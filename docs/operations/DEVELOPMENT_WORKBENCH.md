# Development workbench

Status: current development-only entrypoint. This is not an installed product
launcher and does not authorize physical execution.

The workbench starts the current frontend with Vite hot reload and an empty
ROS execution host. It may activate one exact Scenario project folder.
One command owns file placement, ports, process order, readiness checks,
optional project activation, diagnostics, and cleanup.
Browser mount, reload, and disconnect are read-only reconstruction events.

## Prerequisites

This public runbook is sufficient for operation; no private documentation
checkout is needed. The supported host path uses Linux host networking, Docker
Engine with Compose v2/BuildKit, Git and Python 3. Allow network access for pinned
dependency downloads and enough disk/RAM for the ROS build. This is source-based
workbench setup, not a packaged production installer.

### Install: one build, one start

From a fresh checkout, at the repository root:

```bash
./scripts/compose-dev.sh build   # once, and again after Dockerfile changes
./scripts/compose-dev.sh start   # empty authoring workspace
```

Then open **http://localhost:3000**. The first start also compiles the
ManyForge C++ targets into the image-keyed cache under `.manyforge-dev/`, which
takes a while; later starts are incremental.

`build` prepares the pinned RoboPlan sources under `.docker/`
(`scripts/setup-docker.sh --prepare-only`), then builds
`Dockerfile.ros-lyrical-full` through the `ros-jazzy-full` Compose service into
the image `manyforge-ros-lyrical-full`. It works on x86_64 and arm64. The build
argument `NVIDIA_ISAAC_ROS_RELEASE_SUITE` selects NVIDIA's ROS package suite:
`noble` on x86_64, `noble-jetpack` on a Jetson (Thor, Orin). `build` picks
`noble-jetpack` by itself when `/etc/nv_tegra_release` exists; export the
variable to override it. Building the image by hand needs the same argument:

```bash
./scripts/setup-docker.sh --prepare-only
docker build --build-arg NVIDIA_ISAAC_ROS_RELEASE_SUITE=noble-jetpack \
  -f Dockerfile.ros-lyrical-full -t manyforge-ros-lyrical-full .
```

Plan for well over half an hour on a first build: on a 14-core arm64 Jetson
with the ROS and Python layers already cached it took about 19 minutes; the
resulting image is about 5.4 GB.

The one image serves every lane. It contains ROS 2 Lyrical, the pinned
controller overlay (stock joint_trajectory_controller 6.10.0 with
realtime_tools 5.4.0 and ros2_control_cmake 1.0.0), `topic_based_ros2_control`
for Isaac, and the UFACTORY xArm driver. The driver comes from the fork
`https://github.com/pastoriomarco/xarm_ros2`, branch `lyrical-no-gazebo`
(upstream `lyrical` plus the `xarm_io_states` topic and the `system_control`
service, Gazebo removed), pinned by `XARM_SOURCE_COMMIT` in the Dockerfile and
`manyforge_xarm_control/vendor/driver.yaml`. It covers xArm 5/6/7, Lite 6 and
UF850. The Real lane needs no extra install step.

### Lanes

Each component of a Deployment runs on one lane. All three lanes run
ros2_control with the stock joint trajectory controller, in the same
container:

- **Mock**: ROS 2 mock hardware; no simulator or robot.
- **Isaac**: a scene open in Isaac Sim on this or another machine. Isaac Sim is
  not part of the image and the workbench does not start it, open a stage or
  load a world. Start Isaac Sim, open the scene with its ROS 2 bridge and the
  ROS 2 Simulation Interfaces services (`simulation_interfaces`), then Load
  the Scenario. ManyForge reaches it over DDS (see
  [ROS and DDS](#ros-and-dds)).
- **Real**: a UFACTORY xArm-family robot on the LAN, addressed through the
  site settings ([Real robot setup](REAL_ROBOT_SETUP.md)). Lite 6 is the model
  measured on hardware; physical execution is not qualified and needs
  attended sessions.

A UFACTORY robot is a machine: the owner component carries the lane, and every
member (arm, controller I/O, tool I/O) runs on its owner's lane.

### Environment variables

Export them in the shell that runs `./scripts/compose-dev.sh`; `start` reads
them once, when it creates the container.

| Variable | Default | Effect |
| --- | --- | --- |
| `MANYFORGE_DEV_IMAGE` | `manyforge-ros-lyrical-full` (or `MANYFORGE_ROS_FULL_IMAGE`) | Image that `start` runs. |
| `MANYFORGE_ROS_FULL_IMAGE` | `manyforge-ros-lyrical-full` | Image tag that `build` writes and `start` runs; set it for both. |
| `NVIDIA_ISAAC_ROS_RELEASE_SUITE` | `noble`, or `noble-jetpack` on a Jetson | NVIDIA package suite for `build`. |
| `MANYFORGE_DEV_CACHE_BASE` | `<checkout>/.manyforge-dev` | Build, npm, state and runtime caches. |
| `MANYFORGE_DEV_RUNTIME_STATE_ROOT` | `<cache base>/runtime/<container name>` | Absolute host path of the runtime state root. |
| `MANYFORGE_DEV_SITE_SETTINGS` | unset | Absolute host path of the [site settings](#site-settings) file. |
| `MANYFORGE_DEV_CONTAINER_NAME` | `manyforge-development-workbench` | Container name; use it for diagnosis too. |
| `MANYFORGE_DEV_BUILD_JOBS` | CPU count, at most 8 | Parallel C++ build jobs at start; lower it if compilation runs out of memory. |
| `MANYFORGE_DEV_START_TIMEOUT_SECONDS` | `900` | How long `start` waits for readiness. |
| `ROS_DOMAIN_ID`, `ROS_AUTOMATIC_DISCOVERY_RANGE`, `ROS_STATIC_PEERS`, `RMW_IMPLEMENTATION` | ROS defaults (domain 0, `SUBNET`) | DDS topology; see [ROS and DDS](#ros-and-dds). |
| `MANYFORGE_AI_*` | unset | Tokens for `ai_model` components, forwarded by name ([assistant cookbook](ASSISTANT_E2E_COOKBOOK.md)). |
| `MANYFORGE_MOTION_TRACE_DIR` | `/tmp/manyforge-state/motion-traces` | Motion-trace snapshots for the debug export, in a capped store; empty keeps traces inline ([Reference](#reference)). |

### Site settings

Machine facts live in one JSON file, `manyforge.site.v1`, never in a
Scenario: robot addresses (`device_addresses`) and the host project folders
shown in Composer (`project_folders`). For example:

```json
{"schema":"manyforge.site.v1","device_addresses":{"lite6_main":"192.168.1.31"},"project_folders":["/home/USER/Projects"]}
```

`schema` is required; `device_addresses` and `project_folders` are optional;
no other field is accepted. A Scenario names a `device_addresses` key (for
example `lite6_main`), never the address. Set `MANYFORGE_DEV_SITE_SETTINGS`
to the file's absolute host path before `start`. The file is read when the
workbench starts: after editing it, `stop` and `start` again. `start` refuses
a relative path, a missing file and an invalid file (see
[Site file errors](TROUBLESHOOTING.md#site-file-errors)). Example:
[`../examples/site/manyforge.site.example.json`](../examples/site/manyforge.site.example.json).

### ROS and DDS

The container uses the host network, so DDS sees the host's interfaces. The
launcher passes `ROS_DOMAIN_ID`, `RMW_IMPLEMENTATION`,
`ROS_AUTOMATIC_DISCOVERY_RANGE`, `ROS_STATIC_PEERS`, the Fast DDS and Cyclone
DDS profile variables (`FASTRTPS_DEFAULT_PROFILES_FILE`,
`FASTDDS_DEFAULT_PROFILES_FILE`, `ROS_DISCOVERY_SERVER`, `ROS_SUPER_CLIENT`,
`CYCLONEDDS_URI`) into the container unchanged, and only when they are set.

- `ROS_DOMAIN_ID`: ROS uses 0 when it is unset. Isaac Sim, any other ROS
  machine and the workbench must use the same domain.
- `ROS_AUTOMATIC_DISCOVERY_RANGE`: leave it unset, or `SUBNET` (the ROS
  default), when Isaac Sim or another ROS machine on the LAN must be reached.
  `LOCALHOST` keeps discovery on this machine: use it to isolate a test
  machine, or together with `ROS_STATIC_PEERS` listing every host to reach.
- `FASTDDS_BUILTIN_TRANSPORTS`: the launcher sets `UDPv4` unless it is set,
  so Fast DDS uses UDP loopback instead of shared memory. A shared-memory port
  of a killed process (a hung `ros2_control_node` stopped by SIGKILL) stays
  behind: long-lived processes keep writing into the old port, and a
  replacement that takes its port never receives their requests. With
  `ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST`, rmw_fastrtps always adds shared
  memory and ignores this variable.
- A DDS profile file must exist at the same path inside the container; a
  host-only path does not become visible by exporting it.

Configure the topology before `start`; a running container keeps the values it
was started with.

The launcher builds current C++ owners, the activation preparer and a small
canonical-empty bootstrap from this checkout. It does not import a maintained
package or installed Scenario catalog, and it does not run tests or proofs.
The setup script may recreate stale or modified dependency directories under
`.docker/`; preserve wanted local dependency edits before running it.
Selected projects are read through the ordinary Scenario-project loader and
validated before activation.

### Project folders

To use project folders outside the repository in Composer, list existing
absolute host directories under `project_folders` in the
[site settings](#site-settings) file (keep its `device_addresses` when adding
folders). Each folder must exist when the workbench starts; a path cannot
contain `:`, a folder name cannot contain `,`, and a folder may be listed only
once. Put projects in one of those host folders. Composer's
Load, Save As and Export browsers show the bundled examples and each configured
folder under `/projects/<folder name>`. For example, a project at
`/home/USER/Projects/cell.scenario` appears as
`/projects/Projects/cell.scenario`. If names collide, use the path shown in
the picker.

When entering a path in the picker, use the displayed container path or a path
relative to the current folder. Pasting a host path is not supported. The
site-file `project_folders` setting shows which host folders are available.
`start --project` still accepts only projects under the repository's `examples/`
directory.

After changing `project_folders`, stop the workbench and start it again. Calling
`start` while it is already running does not update the folder mounts.

The workbench owns one stable internal runtime identity, independent of which
Scenario is selected. Portable `manyforge.deployment_intent.v2` documents do not
contain `cell_id`. Starting empty and starting with a project both establish
the same command-closed ROS host before ordinary Load/Apply. Loading another
Scenario changes its equipment, Scene and Program, not the runtime identity.
Multiple instances competing for the same equipment are unsupported.

### Development ROS control preparation

ROS control is the current Workbench path.
Startup builds the ROS execution host and shared candidate worker in the existing
ROS graph and stages the Manager-owned installed targets. Starting without a
project uses the canonical-empty ROS producer. `--project` requests ordinary
startup Load/Apply; `--bootstrap-project` derives an empty predecessor from the
intended Scenario and opens an empty workspace. The two project options remain
mutually exclusive. Either accepts a Scenario with at least one ros2_control
execution component; I/O provider, pose source and AI model components may sit
next to it. Startup refuses any other component kind.

For example, with the built image:

```bash
./scripts/compose-dev.sh start \
  --bootstrap-project examples/sim_ur10e_pick_and_place.scenario
```

The image entrypoint supplies the ROS and Python environment, including the
image's xArm driver and controller overlays. Startup builds the
mechanical ROS overlay and imports its setup into the service environment;
Manager children inherit that environment. Optional: to add separately
installed ROS dependencies on top of the image, set
`MANYFORGE_ROS_DEPENDENCY_SETUP_SCRIPTS` to a colon-separated, ordered list of
absolute setup-script paths inside the container. Startup sources them before
the backend build and imports the application overlay last for services.
Missing configured scripts fail startup with their exact path. Leave it unset
with the image built above.

The ROS option runs with the launching host user's UID/GID and uses the
image-keyed `/runner/home` and `/runner/build` bind mounts for writable home
and build caches. The repository, caches and persistent state directories must
be writable by that user before startup. This supports hosts whose UID differs
from the full ROS image's development user.

The launcher does not start Isaac Sim, choose DDS peers or load a simulator
world. Isaac object observers are pose-source components of the selected
Deployment; the launcher needs no observer input.

`examples/sim_ur10e_pick_and_place.scenario` is an Isaac UR10e and Robotiq
gripper Scenario. Its 0.5 m/s TCP ceiling is an example setting, not a default
for a missing limit.

## Start, inspect, and stop

### Everyday workflow

Run every command from the repository root.

1. **Start** the workbench with one repository Scenario project folder, or
   with no project for an empty authoring workspace:

   ```bash
   ./scripts/compose-dev.sh start --project examples/mock_dual_arm_workbench.scenario
   ./scripts/compose-dev.sh start
   ```

   Example folder names start with their lane: `mock_`, `sim_` (Isaac) or
   `real_`. `examples/mock_dual_arm_workbench.scenario` is the editable
   everyday UR5e + Lite 6 Scenario; edit and Save it in Composer. It has its
   own Scenario ID, so its stored persistent values and Applies are separate
   from other Scenarios. Tests check the other examples, so Save As a copy
   instead of saving over them.

   The project must be a folder containing `scenario.yaml` inside the
   repository's `examples/` folder, the only root startup accepts. Keep the
   terminal output (`… start --project X 2>&1 | tee start.log`). A failed
   developer start uses the same evidence capture as stop; a failed capture
   retains the stopped container and reports its reason.
   A cached start takes about half a minute; the first start, or one after
   native source changes, compiles and takes much longer.
2. **Check readiness.** A successful start prints the owner-issued current
   Scenario with `"ready": true` and `"unavailable_reasons": []`, then the
   frontend URL. Check the reported Scenario and its readiness, not just
   whether the page loads. Repeat the check at any time with:

   ```bash
   ./scripts/compose-dev.sh status
   ```

3. **Open** `http://localhost:3000`. After any start or restart, refresh a tab
   that was already open; it does not reconnect to a new stack by itself.
4. **Stop** when finished, or before switching to another Scenario project
   (selecting a different project while running is refused):

   ```bash
   ./scripts/compose-dev.sh stop
   ```

**After a host reboot** the workbench does not start by itself. Its container
is left `Exited`; run the start command again, which removes the stale
container on its own. Durable journals, incidents and runtime files remain on
the host as evidence. Every full development workbench restart creates a fresh
empty cell; previous runtime state is never adopted or replayed. An explicitly
supplied `--project` still uses the ordinary startup Load path. Without it, Load
the Saved Scenario in Composer when ready, or press New for an empty Untitled
Scenario; no Activate, Execute or Reset is submitted automatically.

For a backend rollout that requires a restart, run `./scripts/compose-dev.sh stop`
and then `./scripts/compose-dev.sh start` from the rollout checkout, with the
same site and state-root settings. The supported flow captures the stopped
container and selects a new session; do not use `docker restart`
or `docker start` on the previously claimed container. Start without
`--project` when the rollout must not Load a Scenario. Frontend-only source
updates use HMR and do not require a workbench restart.

Runtime owners use `current/` below the mounted state root. Before they start,
the launcher atomically moves the previous `current/` into
`previous-sessions/<session-id>/`, preserving every journal and
their WAL/SHM companions together. Archived files lose write permissions and
are never passed to runtime owners. `history.sqlite3` and its companions stay
at the stable state root; installation settings and persistent Program keys
also retain their existing separate storage.

An unclosed previous session, or readable unresolved work, produces a named
warning in the existing Workbench notification rail. An ordinary verified
shutdown stays quiet when remaining journal evidence is opaque; History records
the unknown families. These facts describe durable work and process shutdown,
not physical rest. History also records that old journal events may not have
been imported. Reports are written durably to `session-reports/pending/`, then
the existing historian appends each idempotently before moving it to
`session-reports/`. A crash retries only those report appends, never old effects.

The host holds the state-root flock across session selection and container
creation; the child holds it until owned services finish. Selection follows
confirmed old-container termination and refuses any live overlapping bind,
including foreign containers and ancestor/child paths. The exact Docker container
ID is recorded under that lock before the child can claim its session; an
unbound occurrence or unavailable Docker authority remains unknown and is
preserved. A direct launcher uses the same lock and selection. If legacy or unclosed direct state has no proof
that its old process set stopped, it preserves that state and explains the
missing proof instead of treating PID absence as authority. Legacy flat roots
move only their exact recognized session entries through a durable transition;
unknown entries or conflicting partial transitions are preserved and reported.

During every start the log shows two error-looking lines before the ROS
execution host is up. They are expected and stop once the start reports ready:

- `Execution event pump unavailable: … execution-command.sock … No such file or directory`
- `Execution event pump unavailable: … live logical-scene projection is unavailable`

### Reference

By default, `start` keeps motion-trace snapshots for debug exports in
`/tmp/manyforge-state/motion-traces` (`MANYFORGE_MOTION_TRACE_DIR`). Set
`MANYFORGE_MOTION_TRACE_DIR=` (empty) before starting for no disk snapshots;
the export then carries the trace inline, newest goals first, bounded by the execution socket's send buffer (twice
`net.core.wmem_max`, about 416 KiB at the Linux default). The directory survives Workbench restarts;
files are limited to 3 MiB each and 64 MiB in total, with oldest-first eviction.
These are snapshots of the bounded volatile window (16 goals, 1,024 controller
samples), with explicit coverage and omission counts, not complete Run journals.
Unavailable or invalid diagnostic files do not fail motion or the debug export.

Frontend source edits update through HMR. Backend
startup incrementally builds the current `manyforge_manager_daemon`,
`manyforge_headless_py`, `manyforge_ros2_control_execution_host`,
`manyforge_roboplan_candidate_worker` and Deployment authoring compiler targets
with their dependencies. It deliberately runs no tests, sanitizers, qualification
matrices, package materializers, or proof generators. `status` reports the exact
owner-issued active tuple.

Every removal path (plain stop, failed developer start, exited-container
replacement and operator stop) first stops the container and confirms it is
stopped. It preserves the last 10,000,000 bytes of Docker console output plus
a small image/mount/exit summary in the actual persistent root's
`evidence/<UTC timestamp>/`. Runtime state stays in its bind mount; it is not
copied into that folder. `stop --keep-evidence` remains accepted and uses this
same policy. The summary excludes container environment variables.

Mounted capture reuses one `.mounted-<container-id>.partial/` folder with an
exact matching receipt. It writes the current stopped-container summary first,
then atomically replaces `docker.log` only after the bounded console capture
succeeds. The summary and tail use two separate replacements, so a failed
retry or crash between them preserves the prior tail; a partial folder can
therefore contain an earlier attempt's tail alongside the current summary.
Completion renames the folder to its UTC timestamp before removing the stopped container.
A capture failure reports its reason and keeps the stopped container. If
another failed capture would retain a second container, the launcher retries an
older stopped container once, only when its partial summary matches its actual ID, image, mounts and this workspace. A mounted
older container is removed after that retry, with the outcome reported.
Unidentified historical containers and containers without their state mount
stay protected: the warning names them and asks for manual export or explicit
removal after investigation. Exit 69 after a capture failure means
the container stopped, capture failed and the stopped container was retained;
it does not mean shutdown failed. The launcher reuses the receipt-proven mounted
partial in place. Do not store user files in ManyForge runtime folders: extra
files and directories inside positively owned captures or artifact epochs may
be deleted with an eligible unit, or removed when a capture is replaced.
Links inside an owned unit are unlinked without following their targets. A
pre-existing mount point inside a unit protects that unit and produces a warning.
Concurrent privileged mount changes are outside the supported operator scope. Unknown ownership and multiple matching historical partials
retain their refusal and protection. Back up wanted evidence elsewhere.

There is no automatic migration or state copy for a container without the
`/tmp/manyforge-state` mount. Capture and replacement preserve that stopped
container and require manual export/removal after investigation. A replacement
also refuses a nonempty destination root; ordinary mounted replacements reuse
their existing root.

The launcher runs one bounded retention pass before a new container starts and after a
confirmed stop, after checking that no running, paused or restarting container
has an overlapping bind. A root flock serializes session selection and service
ownership; the mount inspection is a point-in-time check. Selected roots may have symlinked
ancestors; links leading to a unit folder remain protected, with an
accounting-incomplete warning. Other independently eligible units can
still rotate. Links inside a positively owned unit may be unlinked; their targets
are never followed or deleted. The site
settings are `MANYFORGE_LOG_EVIDENCE_MAX_DISK_PERCENT=10` and
`MANYFORGE_LOG_EVIDENCE_MIN_FREE_BYTES=10000000000`. They are triggers for
cleanup, not authority to delete useful traces. Usage accounts allocated
log/evidence bytes across default roots under `.manyforge-dev/runtime/*` on
the same filesystem; an explicit custom root is accounted separately. The
free-space floor uses the actual filesystem. Docker's existing `50m` × 3
console rotation is independent; this is not machine-wide accounting or
discovery of custom roots.

For space-pressure cleanup, only older completed console captures, superseded generated controller-launch
artifact epochs, and older owned host ROS log sessions are eligible. ROS host
logs use `current/execution-run/ros/<sequence>-<session-start-ns>`, stamped once before
the first workbench service starts under the session-directory lock. The
highest sequence identifies the latest session independently of wall-clock
changes and is always protected; an older
session containing a file at or after that start time is also protected.
Legacy timestamp-only names and malformed or ambiguous names remain protected.
Latest-session discovery lists all top-level names; recursive accounting still
examines at most 1,000 candidate units per pass, with sequenced sessions ahead
of protected legacy names. A failed name listing protects
all observed ROS sessions, while a limited deep inventory does not stop
reclamation of independently qualified older sessions. The stable runtime-wide
`current/execution-run/ros-home` preserves spawner lock coordination across controllers
and epochs and is never rotated. Controller child logs retain their existing
per-artifact `ros` directory. The ROS 2 launch framework's `launch.log` is
stored there and counted with its artifact epoch by pressure-based retention.
External-library file logging is disabled for the simulated PLC, Program
Runner rclpy node and managed-host rclcpp context; console output and `/rosout`
remain enabled. Running a ROS host outside the supported workbench
requires explicit owned `ROS_HOME` and `ROS_LOG_DIR` environment paths; the host
provides no fallback path policy. Console chronology comes from
the generated UTC capture name; artifact chronology comes from the write time
of validated ManyForge component metadata markers. Extra files and directory
timestamp changes do not set this order; a marker write time is not an immutable
creation timestamp. At most eight eligible
deletion attempts run per pass; a failed deletion protects that folder for the
pass and cleanup continues with another eligible folder. The latest capture,
latest artifact epoch per bundle (including equal marker times), partial captures, unknown ownership, and
sibling roots are protected. Sibling roots are rotated only on their own next
start/stop. Live journals, current publication/state, inputs and build caches
are never candidates. Directory inventories observe at most
1000 entries each. Overflow warns that usage is a measured lower bound and
unseen material is protected. Cleanup can consider independently eligible
observed units only when measured usage or the actual filesystem free-space
floor proves pressure, oldest observed first. Protected-only or unresolved
pressure reports a warning and stops cleanup; it never gates motion or
startup.

Previous-session journals are not imported into History. The workbench
normally keeps up to five previous sessions.
Unimported records, including safety records, are not guaranteed to survive
retention. See the [session note](FRESH_CHECKOUT_CHECKLIST.md) for their
location and what to preserve.

Each resolved immutable workbench image gets a separate cache under
`.manyforge-dev/images/`; rebuilding the mutable image tag therefore cannot
reuse native readers or npm dependencies from an incompatible runtime. Override
the cache parent, when needed, with `MANYFORGE_DEV_CACHE_BASE`.

Repeating the same `start` is idempotent. Selecting a different project while
the workbench is running is refused; stop it before switching. A failed new
start stops the container it created and uses the same capture policy before
removing it. Container removal keeps the durable
runtime root and the build, npm, and repository-bootstrap caches.

The complete runtime state root is mounted at `/tmp/manyforge-state` in the
container. Its ordinary host location is
`.manyforge-dev/runtime/<container-name>/`, under `MANYFORGE_DEV_CACHE_BASE` when
configured. It is shared across image updates for that workspace and container
name. Set `MANYFORGE_DEV_RUNTIME_STATE_ROOT` to override it with an absolute host
directory. A start refuses a state root bound by another running ManyForge
container. Give concurrent container names separate roots. On a Jetson that boots from eMMC, place
`MANYFORGE_DEV_CACHE_BASE` or the explicit runtime root on an SSD mount, so
these files stay on the SSD rather than the eMMC home.

The binding takes effect when creating a container. An already running container
keeps its existing mounts until an explicit stop/start; the launcher does not
recreate it automatically. Runtime journals and incident evidence are live
records and are excluded from log/evidence rotation.

Composer settings (viewer display toggles, tree editor link style and start
direction, Motion panel layout and confirmations, and profiles; the gear at
the top right) are installation-specific and survive stop/start. They are kept in
`.manyforge-dev/state/composer/settings.json`, mounted into the workbench as
`/var/lib/manyforge/composer` (`MANYFORGE_STATE_DIR=/var/lib/manyforge`).
Values of `persistent` blackboard keys are owned by the Program runner and kept
beside them in `.manyforge-dev/state/program-runner/persistent-keys.json`
(mounted as `/var/lib/manyforge/program-runner`). These installation settings
remain separate from the persistent runtime root described above.
Stored values survive `stop`/`start` while the applied Program still declares
their key persistent with the same type and fields; a value whose key a
committed Apply removed or restructured is dropped with a notice. The
workbench Manager's state resets with the container, so the first Apply after
a start begins a new publication history; the stored values are simply checked
against that Program. A committed Apply the Program runner never observed
keeps the values and is reported as `applied_program_not_observed` on the
stored-values page and in the run audit: it is a defect to investigate. A
register file written in another format is refused by name
(`persistent_key_register_version_unsupported`) and never rewritten; resetting
the register is a deliberate operator step: move the old file aside before the
next start.
An unreadable settings file is never overwritten silently: Composer uses the
defaults, keeps settings read-only and offers a reset that keeps the file as a
`settings.json.bak-<time>` backup.

The current fixed ports are frontend `3000`, Headless API `9010`, Program
runner `9011`, and internal Composer API `9020`. If one is occupied, startup
fails with a direct diagnostic rather than taking over the process.

## Operation and troubleshooting

Use Composer at `http://localhost:3000` for Program, Scene, Components and
Deployment work. Read component states and Runtime/history before issuing
commands. Apply makes a configuration run; Save persists an artifact. Browser
closure does not Stop the runtime, and an unconfirmed viewer pose must not be
treated as current measured state.

The ordinary authoring artifact is a Scenario project directory with fixed
members: `scenario.yaml`, `deployment.yaml`, and `program.yaml`, plus optional
`sources/` and `assets/`. Open the project
directory to load those parts together. Program parameters always take their
Program defaults: a former project with `overrides.yaml` (or a
`manyforge.scenario.project.v1` manifest) is refused, not read; move any
needed value into the Program default and recreate the project. Older Scenario
projects using assignments must be re-authored with direct references. Save updates
authored intent and source references; Save As creates another project without
copying referenced external sources. Missing/invalid sources block
preparation, while changed compatible sources are reported for review rather
than silently pinned to an old vendor copy. Use Export when a portable project
is required: it copies the admitted source/asset closure and rewrites only the
exported references. Do not use Save As as an asset-collection operation.

### Load, New, Activate, Execute, Run

- **New** empties the cell: it applies the built-in, read-only Untitled
  Scenario (empty Program, Scene and Deployment). A finished or stopped Program
  run is Reset first, every component of the current cell is disabled and its
  processes are stopped, and the Scene is initialized. New is the one way to
  empty the cell; it is refused only while motion runs. Save is disabled for
  Untitled; Save Scenario as writes it to a project folder and continues
  there. Unsaved Untitled changes are not recovered after a Composer restart.
- **Load Scenario** is New, then Apply of the chosen project. Its components
  come up **prepared**, never enabled. When the cell is not empty, New and
  Load ask once, listing what they stop; they also warn about unsaved Draft
  changes.
- **Apply** (Deployment) makes the Draft the running configuration. Replaced
  and removed components are disabled and stopped first; affected components
  come back prepared. Apply is refused while a Program run is in progress.
- **Prepare** and **Enable** run on one component (its page) or on all of them
  (**Prepare all**, **Enable all** in the Components list). On a machine,
  Prepare the owner first; Enable acts on members. On a real xArm the motors
  switch on only at Enable ([Real robot setup](REAL_ROBOT_SETUP.md)).
- **Execute** (Program panel) starts the Applied Program tree. Every active
  component of the Deployment must be enabled. When some are not, Execute
  offers **Activate and execute**: it prepares and enables every active
  component, then executes the Program once.
- **Run** starts motion. A Program starts held: each motion group shows **Run**
  in the Motion panel, and nothing moves until it is pressed. **Stop** holds a
  group, **Resume** continues it, **Halt** stops all Program motion and halts
  the tree, and **Reset** ends the run.
- **Reset robot position** (a Reset option, and **Reset position** on a robot's
  component page) sets the robots to the Deployment's initial joint positions.
  It is offered where every robot's adapter supports it: Mock and Isaac,
  never a real robot. The trajectory controller is switched off, the initial
  joints are written once (on Isaac to its joint commands topic), and the
  controller is switched back on to hold them. On Isaac it is refused with
  **Simulation not playing** while the simulation isn't playing; play it, then
  reset.
- **Recover** (component page) clears a component fault; for a machine, Recover
  the owner, then Enable its arm. A component in fault blocks Execute. When a
  component faults while something executes, or is unhealthy after a Stop,
  the cell is latched ("N errors require attention"): runs, nodes, manual
  motion and manual I/O are blocked until every component is healthy and idle
  again. Component actions, Apply, New and Load still work.

A component that is deactivated in the Deployment (its **Active** box off) is
not part of the running cell: it is never prepared or enabled and blocks
nothing.

On the Isaac lane, a simulation that is not playing never blocks Enable,
Activate or Execute. Its robots and tools keep their lifecycle and show the
reason **Simulation not playing**, and their streams show stale; this is
never a fault and never latches the cell. A new motion or gripper command on
them is refused with `simulation_not_playing`: a Program node fails with that
code and the tree handles it like any failed node. A Program can play the
simulation itself with a **Set simulation state** node
(`simulation.set_state`, `play`) before its motion. The same reason appears when the workbench
cannot reach Isaac at all. In that case the simulator component stays Offline
with `simulator_unreachable` and a warning; its state does not latch the cell
or block Execute. Robot and tool health still follows their own reported
states ([Troubleshooting](TROUBLESHOOTING.md#simulation-not-playing)).

### Components and Deployment

The center-top sections are Program, Scene, Components and Deployment.

**Components** shows the cell. On the left, the list: every component of the
Deployment Draft and of Manager Applied, with its kind and model, a tool's
mount (`lite_gripper → lite6_robot.flange`) and its state, or Added in Draft /
Removed in Draft; a machine's members sit under it. Above the list,
**Prepare all** and **Enable all**; below it, **Add component**. On the right,
the selected component's page:

- what it is: model, mount or mounted tool, Deployment status and the Program
  roles assigned to it, with **Open in Robot Control** (jogging and gripper
  commands stay in the viewer's Robot panel), **Roles in Deployment** and, for
  an unapplied change, **Review in Deployment Changes**;
- its status: lifecycle (Offline, Not prepared, Prepared, Enabled, Fault),
  fault, reason, stream freshness, the last action's result, and the actions
  it offers (Prepare, Enable, Disable, Recover, plus Reset position, Power
  off, Reboot or Reset simulation where the adapter supports them). A
  machine's card also has its **Lane**, **Address (site settings key)**,
  **Controller status** (as the controller reports it) and its **Members**,
  each with an **Active** box;
- placement: base pose, lane (a member runs on its owner's lane), a tool's
  mount and **Primary tool** box, **Remove from Draft**, Preview and Apply;
- its TCPs and frames (see [TCPs](#tcps)), a tool's payload (read-only, from
  its model), its I/O signals (live values stay in the viewer's Tool & I/O
  panel), and its settings in the Deployment Draft: model and target limits,
  execution host configuration, Stop settings, gripper, I/O, pose source, AI
  model and stream overrides, each only when the component has them.

Each action (Prepare, Enable, Disable, Recover, ...) is one adapter action.
Its result is **succeeded**, **failed** with a typed reason, or **unknown**;
nothing resends an action whose effect is unknown. One action runs at a time
per component; a member action is refused while its owner is not prepared
(`owner_not_prepared`). What comes next is decided from the component's
current state. See [Observability](OBSERVABILITY.md#workbench-console).

**Deployment** keeps the cell as a whole, in four tabs:

- **Overview**: the runtime status summary, the Deployment Draft against
  Manager Applied (components to add or remove, runtime convergence, Program
  roles) and the Manager Apply inventory.
- **Roles**: each Program role (satisfied, unbound or incompatible) and the
  Program Binding.
- **Cell settings**: motion takeover timing, global interrupt rules, the AI
  model aliases, pose links and stream overrides that no current component
  owns, and the read-only compiled artifact.
- **Changes**: Preview compiles the Draft and shows the diff against Applied
  (components added, replaced or removed, with the reasons; Program impact)
  without changing anything. Apply sends that exact Draft once; when components
  are brought up, replaced or removed, Manager asks for one confirmation.
  When a finished or stopped run blocks Apply, **Reset and Apply** resets it
  first. Apply also refuses with `component_action_in_progress` when a
  component it would replace or remove has a lifecycle action in progress;
  retry after that action finishes.

**Add component** (Components) adds components that already exist: the
complete rows of the example Scenarios under `examples/` (machines, robots,
tools, I/O devices, pose sources, AI models). Adding a machine adds its owner
and every member (ids `<owner_id>_<role>`). Pick the kind and the entry, check
the component id (the id a Program role is assigned to binds that role), and
for a robot the frame prefix and base position; for a tool, pick the robot
under **Mount on**; then **Add to Draft**. The component is only in the Draft
until Apply; Discard undoes it. Component models without a ready row are
listed by name; write the row in a Scenario first
([Adding a component](../authoring/ADDING_A_COMPONENT.md)). An I/O device or
AI model that no example has is written by hand under **Custom I/O device or
AI model**.

Scripts and assistants use the same routes: `GET
/api/deployment/component-catalog` lists the entries (MCP
`manyforge_deployment` action `component_catalog`); `POST
/api/deployment/draft/components` adds `components` and `attachments` rows in
one edit (action `add_components`); `POST /api/deployment/draft/mutations`
changes or removes existing rows (action `mutate`); `POST
/api/deployment/preview` returns the diff and `POST /api/deployment/apply`
applies it. `POST /api/scenario/load` and `POST /api/scenario/new` are Load and
New (MCP `manyforge_scenario`); `GET /api/components` and `POST
/api/components/{component_id}/actions` read states and run actions (MCP
`manyforge_components`).

Inspect before restarting:

```bash
./scripts/compose-dev.sh status
docker logs --tail 200 manyforge-development-workbench
docker inspect --format '{{.State.Status}} {{.State.ExitCode}}' manyforge-development-workbench
```

Use the selected `MANYFORGE_DEV_CONTAINER_NAME` if overridden. Child-process
output is inherited by the container log. Stop keeps durable runtime state
and captures the bounded console tail before removal. Capture any additional
logs/status needed for investigation explicitly. Failed developer startup uses
the same capture policy; retain the startup terminal output too.

### TCPs

The rules (which TCPs a robot lists, the End-effector and Active TCP, Set TCP,
the controller's TCP offset) are in the [TCP guide](../authoring/TCPS.md).
In the workbench:

- A component page lists its **TCPs and frames**: model and descriptor TCPs
  (read-only) and **custom TCPs** of a robot or tool, which you add, edit and
  delete there (an id, a parent frame of that component, a position in metres
  and roll/pitch/yaw in degrees). Each change is a Deployment Draft edit (the
  row's `custom_tcps`) and runs after Apply.
- A component with TCPs on several targets and no single declared default
  shows a **Default TCP and primary tool** card to choose its default TCP.
  When several tools share one mount, the tool whose mount has **Primary
  tool** checked gives the robot its End-effector TCP.
- **Robot Control** has a **TCP** select beside the robot that shows and sets
  its Active TCP. Jog, Move to, the Cartesian moves and the viewer gizmo use
  it. A robot without an End-effector TCP starts with no TCP selected and
  offers joint jog only.
- **Programs** choose each move's TCP as **Current** (the Active TCP),
  **Default TCP of <role>**, or a specific TCP of the bound robot or a tool.
  Custom TCPs still in the Draft are marked "after Apply".

### When an Apply or Load fails

An Apply that fails reports each affected component's outcome. Nothing is
rolled back or retried: the new Deployment stays installed, every affected
component is still prepared (a member of a failed owner reports
`owner_not_prepared`), and you fix what failed with the component actions
(Prepare, Recover, Prepare all). An Apply that fails before it changes
anything leaves authoring open; correct the named cause and Apply again. An
Apply that fails after it started changing the cell (effect unknown) keeps
Apply refused (`authoring_not_ready`) until **New** or **Load** (which starts
with a New): New stops and removes everything in the cell, whatever the failed
Apply installed. Stop and the installed Program keep working meanwhile. Do
not restart the workbench merely because an Apply failed.

One Load refusal needs a restart: Composer shows **Scenario refused**
(`scenario_composition_refused`) when the execution host finds an
inconsistency while composing the Scenario. Nothing was applied. Fix the cause
named in the details, then restart the workbench before the next Load:

```bash
./scripts/compose-dev.sh stop --keep-evidence
./scripts/compose-dev.sh start --project 'examples/<saved>.scenario'
```

`--project` accepts only Scenario project folders under `examples/`; for a
project elsewhere, start without it and Load it in Composer. The page
disconnects while the container is replaced and may need a refresh; unsaved
Draft changes are lost. A restart does not start a Program or authorize
motion.

These steps are for the workbench. They are not a containment or recovery
procedure for physical equipment.

| Symptom | Action |
| --- | --- |
| Image missing | Run `./scripts/compose-dev.sh build` (see [Install](#install-one-build-one-start)). |
| Site settings refused at start | See [Site file errors](TROUBLESHOOTING.md#site-file-errors). |
| Project source missing, invalid or changed | Preserve the diagnostic, inspect the named project/source path, and review changes before retrying. Do not bypass identity checks or delete caches blindly. |
| Port occupied | Identify the listener on 3000/9010/9011/9020; do not kill an unrelated process. |
| Different or unhealthy workbench already running | Read status/logs first. Stop only with authorization before switching scenarios; do not spawn another stack to hide the failure. |
| Slow first start | Inspect build output. Lower `MANYFORGE_DEV_BUILD_JOBS` if compilation exhausts memory; avoid repeated startup attempts. |
| `status` says stopped after a failed start | The launcher removed the container it created, with its logs. Read the start output you kept (`tee`), or start again with its output captured. |
| `Scenario project is outside allowed roots` (exit 69) | Pass a project folder inside `examples/`, the only root startup accepts; a scratch folder such as `.manyforge-dev/` is refused. |
| Start refuses with `… assistant instrument source is invalid or stale` or `… node environment manifest is stale` | A generated source is out of date after a catalog or covered source change. Run `python3 scripts/regen.py --update` in the workbench image, review the diff, then `python3 scripts/regen.py --check` ([Identities and pins](../reference/IDENTITY_AND_PINNING.md)). |
| Error-looking `Execution event pump unavailable` lines during start | Expected before the ROS host is up; see the everyday workflow. Investigate only if they continue after the start reports ready. |
| Isaac robot shows **Simulation not playing** | Play the simulation, or check that the workbench reaches Isaac ([Troubleshooting](TROUBLESHOOTING.md#simulation-not-playing)). |
| Real robot does not connect | See [Real robot setup](REAL_ROBOT_SETUP.md#troubleshooting). |
| Page stale or disconnected after a restart | Refresh the tab. |
| Stale viewer or failed command | Inspect Runtime/history and component states. Do not infer rejection from a lost response or resend motion/Apply automatically. |

For API clients, use the public generated
[Composer contract](../reference/REST_API.md),
[Headless contract](../reference/HEADLESS_REST_API.md) and
[optional MCP contract](../reference/MCP_RUNTIME_CONTRACT.md).
Running OpenAPI schemas are at `http://localhost:9020/openapi.json` and
`http://localhost:9010/openapi.json`. Schema availability is not readiness.
Host networking and the frontend's `0.0.0.0` binding require a trusted network
or appropriate firewall/access controls; do not expose this workbench publicly.

## Classification and limits

When a project is active, `status` reports its owner-issued current identity and
convergence. It reports a `development_workbench` using repository source, while
`physical_execution_authorized` remains false. The Workbench prepares ROS
execution adapters and does not authorize physical equipment. There is no
installer or official AMD64/ARM64 package; the workbench runs from source.
