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
checkout is needed. This is a source-based workbench, not a packaged
production installer.

- **Host**: Linux with Docker host networking, x86_64 (amd64) or arm64.
  Ubuntu 24.04 is the reference; Jetson AGX Thor and Orin run JetPack 7.2.1.
  Docker Desktop on macOS or Windows is not supported.
- **Software**: Docker Engine, Git and Python 3. Building the image
  ([option 2](#option-2-build-the-image-from-source)) also needs the Compose
  v2 plugin and BuildKit.
- **Network**: to pull or build the image, and at the first start, which
  downloads the frontend's npm packages.
- **Memory**: 16 GB for the C++ build at the first start; lower
  `MANYFORGE_DEV_BUILD_JOBS` if compilation runs out of memory.
- **Disk**: the image is about 4.1 GB; build caches and runtime state grow
  under `.manyforge-dev/` in the checkout. `start` refuses to run with less
  than 10 GB free ([Disk limits](#disk-limits)).

<a id="install-one-build-one-start"></a>

## Install

Get the source. `main` holds the latest release (`dev` is the integration
branch). Run every command in this guide from the repository root:

```bash
git clone https://github.com/pastoriomarco/manyforge.git
cd manyforge
```

ManyForge runs in one container image, tagged locally
`manyforge-ros-lyrical-full`. Options 1 and 2 differ only in where that image
comes from; both then use the same `start`.

| Option | Use it when | Commands | Before the first start |
| --- | --- | --- | --- |
| **1. Pull the published image** (default) | You want to run ManyForge on amd64 or arm64 (Jetson AGX Thor and Orin included). | `./scripts/compose-dev.sh pull`, then `./scripts/compose-dev.sh start` | The download (the image is about 4.1 GB). |
| **2. Build the image from source** | You changed `Dockerfile.ros-lyrical-full` or what it installs, your checkout is not a release (for example `dev` after such a change), or the host cannot reach `ghcr.io`. | `./scripts/compose-dev.sh build`, then `./scripts/compose-dev.sh start` | About an hour of building. |
| **3. Operator launcher** (advanced) | The stack must run from a digest-checked package with Composer served on the LAN at port 9000, or on a remote Docker host next to Isaac Sim. | `manyforge-operator start …` ([operator runbook](MANYFORGE_OPERATOR_RUNBOOK.md)) | The image from option 1 or 2, plus the package preparation in the runbook. |

### Option 1: pull the published image

GitHub builds the reference image from this repository with each release,
for linux/amd64 and linux/arm64 (Jetson AGX Thor and Orin, JetPack 7.2.1),
and publishes it as `ghcr.io/pastoriomarco/manyforge-ros:<VERSION>`. Pulling
needs Docker and network access to `ghcr.io`; no login.

```bash
./scripts/compose-dev.sh pull
./scripts/compose-dev.sh start
```

`pull` fetches the tag named by the checkout's `VERSION` file (for example
`ghcr.io/pastoriomarco/manyforge-ros:0.3.1`) and tags it as
`manyforge-ros-lyrical-full`, the image `start` runs. To pin one exact image,
pass its reference or its digest:

```bash
docker image inspect --format '{{index .RepoDigests 0}}' manyforge-ros-lyrical-full
./scripts/compose-dev.sh pull ghcr.io/pastoriomarco/manyforge-ros@sha256:<digest>
./scripts/compose-dev.sh pull sha256:<digest>
```

The first command prints the digest of the image a `pull` fetched; the other
two (equivalent) fetch exactly that image again, on this or another machine.

### Option 2: build the image from source

```bash
./scripts/compose-dev.sh build
./scripts/compose-dev.sh start
```

`build` takes about an hour. It prepares the pinned RoboPlan sources under
`.docker/` (`scripts/setup-docker.sh --prepare-only`; this may recreate stale
or modified dependency directories there, so preserve wanted local edits
first), then builds `Dockerfile.ros-lyrical-full` through the
`ros-jazzy-full` Compose service into the same tag,
`manyforge-ros-lyrical-full`. It needs the Compose v2 plugin, BuildKit and
network access. It is the Dockerfile and default build arguments the
published image is built from, and it works on x86_64 and arm64. Run it
again after a Dockerfile change; unchanged layers come from Docker's cache.

ROS 2 Lyrical is built from source by default (`ROS_BASE=source`), the same
way on every host, a Jetson included. `MANYFORGE_ROS_BASE=apt` installs ROS
from NVIDIA's Isaac ROS apt mirror instead and adds the pinned controller and
`rmw_fastrtps` overlays. Only that apt base reads
`NVIDIA_ISAAC_ROS_RELEASE_SUITE`: `noble` on x86_64, `noble-jetpack` on a
Jetson, which `build` selects by itself when `/etc/nv_tegra_release` exists
(export the variable to override it).

Building the default image by hand (BuildKit required):

```bash
./scripts/setup-docker.sh --prepare-only
docker build -f Dockerfile.ros-lyrical-full -t manyforge-ros-lyrical-full .
```

### Option 3: operator launcher (advanced)

`manyforge-operator` starts the same application from a digest-checked
package and serves the built Composer on the network at port 9000, without
Vite. Use it only for the cases in the table above; it needs the image from
option 1 or 2 on its runtime host (`isaac` profile) or a host ROS install
(`standard` profile), and it cannot configure real-robot addresses. Follow
the [operator runbook](MANYFORGE_OPERATOR_RUNBOOK.md).

### First start and what to check

```bash
./scripts/compose-dev.sh start --project examples/mock_lite6_pick_and_place.scenario
./scripts/compose-dev.sh status
```

Omit `--project` for an empty authoring workspace. The image holds no
ManyForge program: before it starts the workbench, `start` compiles
ManyForge's C++ programs and Python modules from the checkout into the
image-keyed cache under `.manyforge-dev/images/<image id>/`, and the first
start also installs the frontend's npm packages. The first start therefore
takes much longer than later ones (a cached start takes about half a
minute); the build output streams to the console and does not count against
the startup timeout.

Check that it works:

1. `start` ends by printing the current Scenario with `"ready": true` and
   `"unavailable_reasons": []`, then `Development workbench:
   http://localhost:3000`. `./scripts/compose-dev.sh status` prints the same
   JSON at any time.
1. Open **http://localhost:3000**. With the example project, press
   **Execute**, then **Run**: the Lite 6 picks the part and places it.
1. `docker image inspect --format '{{.Id}}' manyforge-ros-lyrical-full`
   names the image in use.

Starting the workbench runs no tests and authorizes no robot motion. Stop it
with `./scripts/compose-dev.sh stop`. The
[fresh-checkout checklist](FRESH_CHECKOUT_CHECKLIST.md) is a recorded
version of these checks.

### Update to a new version

```bash
./scripts/compose-dev.sh stop
git pull
./scripts/compose-dev.sh pull     # option 2: ./scripts/compose-dev.sh build
./scripts/compose-dev.sh start
```

`git pull` on `main` moves to the latest release; `pull` then fetches that
version's image. With option 2, `build` is needed only when the Dockerfile
or what it installs changed. A new image has a new ID, so the first start
on it compiles ManyForge's programs again into its own cache folder; the
previous image's folder stays in `.manyforge-dev/images/` and can be deleted
while the workbench is stopped. Saved Scenarios are kept, and so are the
Composer settings and runtime state under `.manyforge-dev/`.

### Fall back

- **`pull` fails** (no network, `ghcr.io` unreachable, or no image published
  for the checkout's `VERSION`): use option 2. `pull` and `build` write the
  same tag, so the one run last is the image `start` uses.
- **`start` fails after an image change**: read the start output first
  ([troubleshooting table](#operation-and-troubleshooting)). To return to
  the image you had, pull it again by its digest (option 1), or check out the
  previous source and `build`. Each image keeps its own build cache, so going
  back reuses it.
- **To return to an earlier release**: stop the workbench, check out its tag
  (`git checkout vX.Y.Z`) and follow the guide in that checkout. Releases
  before 0.3.1 have no published image and are built with option 2.

### What the image holds

The one image serves every lane. It holds Ubuntu 24.04, ROS 2 Lyrical built
from source with ros2_control and the stock joint_trajectory_controller
6.10.1, `topic_based_ros2_control` for Isaac, the UFACTORY xArm driver,
RoboPlan with its dependencies, the Python environment, and Node.js with
npm. It holds no ManyForge program and no built frontend: both are built
from the mounted checkout. The owners' licence files are in the image under
`/usr/share/licenses/manyforge-image/`, and
[THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md) lists every component
it redistributes.

The xArm driver comes from the fork
`https://github.com/pastoriomarco/xarm_ros2`, branch `lyrical-no-gazebo`
(upstream `lyrical` plus the `xarm_io_states` topic and the `system_control`
service, Gazebo removed), pinned by `XARM_SOURCE_COMMIT` in the Dockerfile and
`manyforge_xarm_control/vendor/driver.yaml`. It covers xArm 5/6/7, Lite 6 and
UF850. The Real lane needs no extra install step.

## Lanes and configuration

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
| `MANYFORGE_DEV_IMAGE` | `manyforge-ros-lyrical-full` (or `MANYFORGE_ROS_FULL_IMAGE`) | Local image tag that `build` and `pull` write and `start` runs; set it for all three. |
| `MANYFORGE_ROS_FULL_IMAGE` | `manyforge-ros-lyrical-full` | The same tag, used when `MANYFORGE_DEV_IMAGE` is unset. |
| `MANYFORGE_ROS_BASE` | `source` | ROS base that `build` uses: `source` (ROS 2 Lyrical built from source) or `apt` (NVIDIA's Isaac ROS apt mirror). |
| `NVIDIA_ISAAC_ROS_RELEASE_SUITE` | `noble`, or `noble-jetpack` on a Jetson | NVIDIA package suite for `build`, read only with `MANYFORGE_ROS_BASE=apt`. |
| `MANYFORGE_DEV_CACHE_BASE` | `<checkout>/.manyforge-dev` | Build, npm, state and runtime caches. |
| `MANYFORGE_DEV_RUNTIME_STATE_ROOT` | `<cache base>/runtime/<container name>` | Absolute host path of the runtime state root. |
| `MANYFORGE_DEV_SITE_SETTINGS` | unset | Absolute host path of the [site settings](#site-settings) file. |
| `MANYFORGE_DEV_CONTAINER_NAME` | `manyforge-development-workbench` | Container name; use it for diagnosis too. |
| `MANYFORGE_DEV_BUILD_JOBS` | CPU count, at most 8 | Parallel C++ build jobs in the native build step at start; lower it if compilation runs out of memory. |
| `MANYFORGE_DEV_START_TIMEOUT_SECONDS` | `900` | How long `start` waits for readiness after the native build step; build time does not count. |
| `ROS_DOMAIN_ID`, `ROS_AUTOMATIC_DISCOVERY_RANGE`, `ROS_STATIC_PEERS`, `RMW_IMPLEMENTATION` | ROS defaults (domain 0, `SUBNET`) | DDS topology; see [ROS and DDS](#ros-and-dds). |
| `MANYFORGE_AI_*` | unset | Tokens for `ai_model` components, forwarded by name ([assistant cookbook](ASSISTANT_E2E_COOKBOOK.md)). |
| `MANYFORGE_MOTION_TRACE_DIR` | `/tmp/manyforge-state/motion-traces` | Motion-trace snapshots for the debug export, in a capped store; empty keeps traces inline ([Reference](#reference)). |

### Site settings

Machine facts live in one JSON file, `manyforge.site.v2`, never in a
Scenario: robot addresses (`device_addresses`), the host project folders
shown in Composer (`project_folders`) and the [disk limits](#disk-limits).
For example:

```json
{"schema":"manyforge.site.v2","device_addresses":{"lite6_main":"192.168.1.31"},"project_folders":["/home/USER/Projects"]}
```

`schema` is required; every other member is optional, and no other field is
accepted:

| Member | Default | Meaning |
| --- | --- | --- |
| `device_addresses` | none | Robot addresses by key. |
| `project_folders` | none | Host folders shown in Composer. |
| `disk_max_percent` | `10` | The largest share of the disk ManyForge's own files may take, in percent; an integer from 1 to 100. |
| `disk_always_free_bytes` | `10000000000` (10 GB) | The space that always stays free, in bytes; an integer from 0 to 9007199254740991. |
| `disk_machine_minimum_bytes` | `0` | The space the machine itself needs besides ManyForge, in bytes, added to the free space a start needs; not determined yet. Same range. |

A missing member takes its default, so a file only needs the members it
changes; without a file every default applies. A `manyforge.site.v1` file is
still accepted and uses the default disk limits; a file with a disk member
must say `manyforge.site.v2`. A Scenario names a `device_addresses` key (for
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

For example:

```bash
./scripts/compose-dev.sh start \
  --bootstrap-project examples/sim_ur10e_pick_and_place.scenario
```

The image entrypoint supplies the ROS and Python environment, including the
image's xArm driver. Startup builds the
mechanical ROS overlay and imports its setup into the service environment;
Manager children inherit that environment. Optional: to add separately
installed ROS dependencies on top of the image, set
`MANYFORGE_ROS_DEPENDENCY_SETUP_SCRIPTS` to a colon-separated, ordered list of
absolute setup-script paths inside the container. Startup sources them before
the backend build and imports the application overlay last for services.
Missing configured scripts fail startup with their exact path. Leave it unset
with the standard image.

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

Inside the container the Manager is the one supervisor of the application:
it starts Headless, the Program runner and Composer (Composer once its first
empty baseline is ready), and owns the execution host. If any of them exits,
the Manager ends the whole application and the workbench stops; the console
shows `<process> exited with status N`, then
`development_workbench_unavailable: Manager ended the application with status 72`. Nothing is restarted on its
own: read the log, then start again. `stop`, and every other end of the
application, first asks for a Halt of the cell and waits until it is
complete, then stops Composer, the Program runner and Headless and waits for
each to exit, then the execution host, and last the Manager writes its own
records. No step has a time limit: the log shows the step being waited for
(`application stop: waiting at <step>: …`, and the execution host's
`native shutdown entering/waiting at <phase>`). A second stop request (a
second SIGTERM or Ctrl+C to the workbench) ends the processes still running:
the log names the step (`application_stop_forced`) and each process ended
(`application_process_killed`), and the Manager still writes its own records.
`compose-dev.sh stop` gives the container 30 s; Docker then ends whatever is
left. A Halt that is refused or cannot be followed is logged as
`manager_planned_stop_overrun` or `execution_host_planned_stop_overrun`;
check that the robot has stopped before starting again. If a step of the stop
did not complete, the Manager ends with status 74 and the next start reports
that the shutdown was unclean or could not be verified.

Runtime owners use `current/` below the mounted state root. Before they start,
the launcher atomically moves the previous `current/` into
`previous-runs/<session-id>/`, preserving every journal and
their WAL/SHM companions together. Archived files lose write permissions and
are never passed to runtime owners. History stays in `history/` at the stable
state root; installation settings and persistent Program keys also retain
their existing separate storage. A state root that still holds
`previous-sessions/` from an earlier build, or any other entry the launcher
does not know, is refused and left unchanged; move that entry out of the
state root, then start again.

An unclosed previous session, or readable unresolved work, raises the alarm
"ManyForge did not close correctly last time"
(`manyforge.runtime.previous_session_unclean`) once Headless has filed the
previous-session report at start. An ordinary verified shutdown stays quiet
when remaining journal evidence is opaque; History records the unknown
families. These facts describe durable work and process shutdown,
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
files are limited to 5 MiB each and 64 MiB in total, with oldest-first eviction.
These are snapshots of the bounded volatile window (16 goals and the last 2,340
controller samples: about 15 s at 150 Hz for one arm, about 8 s for two), with
explicit coverage and omission counts, not complete Run journals.
Unavailable or invalid diagnostic files do not fail motion or the debug export.

Frontend source edits update through HMR. Before it starts the workbench
container, `start` runs the native build step,
`python3 scripts/dev-workbench.py build`, in a short-lived container with the
same image, user, build settings and source/cache mounts. It incrementally
builds the current `manyforge_ros2_control_execution_host`,
`manyforge_roboplan_candidate_worker`, `manyforge_manager_daemon`,
`manyforge_headless_py` and `manyforge_core_py` targets with their
dependencies in one CMake tree (`/runner/build/ros-integration`), so Core,
Deployment and the other shared packages compile once. Only the Deployment
authoring tools (`manyforge_prepare_activation`,
`manyforge_generate_canonical_empty_package`) build in their own tree
(`/runner/build/deployment-authoring`): they stay statically linked because
the workbench keys its bootstrap package cache by their bytes. Its output
goes to the `start` console (stderr); if it fails,
`start` exits without starting the workbench. Only then does the readiness wait
(`MANYFORGE_DEV_START_TIMEOUT_SECONDS`) begin. The workbench itself never
builds: it checks these artifacts and refuses, naming the missing file and the
build step to run, when one is absent. Apply and Load never build.
`./scripts/compose-dev.sh build-native` runs the same build step on its own,
for example to build before a timed start. In operator mode (the
[operator runbook](MANYFORGE_OPERATOR_RUNBOOK.md)'s `isaac` profile), where
Composer serves the built GUI on port 9000 without Vite, the build step also
builds the frontend from the mounted source into
`manyforge_composer/frontend/dist` (`npm run build`), skipped while that
build matches the frontend source; the image carries Node.js and npm only. The build
step deliberately runs no tests, sanitizers, qualification matrices, package
materializers, or proof generators. `status` reports the exact owner-issued
active tuple.

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
are never followed or deleted. The disk
limits are the site settings' `disk_max_percent` (default 10) and
`disk_always_free_bytes` (default 10 GB); see [Disk limits](#disk-limits).
They are triggers for
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

Previous-run journals are not imported into History. Previous runs are
pruned at each start, before the previous `current/` moves, to five runs,
counting that `current/`, and to 2 GiB of archived runs and older History
files together, oldest first; the newest archived run, the run the current
report describes and the current History file are never deleted. See
[Disk limits](#disk-limits) for the pruning above those limits.

#### Disk limits

ManyForge's own files under the runtime state root take at most
`disk_max_percent` (default 10) percent of the disk, and
`disk_always_free_bytes` (default 10 GB) always stay free; both are
[site settings](#site-settings). While
either limit is exceeded, the start also deletes previous runs and older
History files below the five-run and 2 GiB limits, oldest first, at most
eight per pass, with the same protections. While the workbench runs, it
repeats this pass every 60 seconds, only when a limit is exceeded; the
current run's files are never deleted. Each deletion is one line in the
container log, naming the folder and the limits.

After that pass, if the disk that holds the runtime state root (or its
`history/`) has less free space than `disk_always_free_bytes` plus
`disk_machine_minimum_bytes` (10 GB by default), or no free inodes, `start`
stops before anything is written, with exit status 69:

```text
disk_space_below_floor: Not started: the disk that holds <path> has 4 GB free. ManyForge needs at least 10 GB free (10 GB plus 0 GB for the machine). Free 6 GB and start again.
```

`MANYFORGE_LOG_EVIDENCE_MAX_DISK_PERCENT` and
`MANYFORGE_LOG_EVIDENCE_MIN_FREE_BYTES` are no longer read: `start` refuses
to run while either is set and names the site settings member to use. The report of the previous run lists up to 64 of its
unfinished operations, each marked `host_replaced`. In the execution host's
operation journal it counts as unresolved an unfinished operation, an
accepted Stop without a completion row, and a cell-admission claim with a
`cell_claim_acquired` row but no `cell_claim_released` row. A journal whose
`journal_metadata.records_lost` is 1 lost records during that run (the alarm
"Operation records are being lost" was shown), so such counts can be
incomplete or wrong.
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
beside them in `.manyforge-dev/state/program-runner/` (mounted as
`/var/lib/manyforge/program-runner`), in a file named by its format version,
`persistent-keys.v<N>.json` (currently `persistent-keys.v5.json`). These installation settings
remain separate from the persistent runtime root described above.
Stored values survive `stop`/`start` while the applied Program still declares
their key persistent with the same type and fields; a value whose key a
committed Apply removed or restructured is dropped with a notice. The values
follow the Program the runner actually activates: on every wake it checks them
against the current committed activation, and an Apply a later one superseded
before the runner activated it has no effect. Only
the current format's file is opened or created. An earlier register file
(`persistent-keys.json`, or a `persistent-keys.v<N>.json` with a lower number)
is never opened, moved, rewritten or deleted; when the current file does not
exist yet the stored values start empty and the Program runner logs one line
naming the earlier file. A current-format file whose content names another
format (a damaged or hand-edited file) is refused by name
(`persistent_key_register_version_unsupported`) and never rewritten; move it
aside before the next start to begin with no stored values.
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
- **Apply** and **Revert**, of the Program or the Deployment, keep the live
  Scene: objects stay where they are; only Load and New initialize it. The
  confirmation offers **Reload initial scene** (off by default) on every
  Deployment Apply and Revert, and on a Program Apply or Revert when the
  Program's initial scene changes. Once the Apply or Revert is Applied, a
  Reset with **Reload initial scene** follows, which puts every object back
  to its start pose and returns persistent keys to their initial values.
  While it waits for the cycle to be Idle, the notice **Initial scene reload
  pending** is shown. If a run or another command starts first, the Reset is
  not sent and **Initial scene not reloaded** stays until acknowledged. If
  that Reset is refused or fails, the Apply stays and the Reset's error is
  shown. An Apply or Revert that is refused or fails reloads nothing.
  Without the option, the live cell keeps its current scene until a
  Reset with **Reload initial scene**. While a finished, stopped or held run
  exists, Apply and Revert Reset it first (**Reset run and Apply**, **Reset
  run and Revert**).
  - A Deployment Apply that removes a component carrying an object fails
    (`configuration_preservation_failed`). Reset with **Reload initial
    scene** first, then apply.
  - Objects placed relative to a component that the Apply moves keep their
    old cell pose until a Reset with **Reload initial scene**.
- **Prepare** and **Enable** run on one component (its page) or on all of them
  (**Prepare all**, **Enable all** in the Components list). On a machine,
  Prepare the owner first; Enable acts on members. On a real xArm the motors
  switch on only at Enable ([Real robot setup](REAL_ROBOT_SETUP.md)).
- **Execute** (Program panel) starts the Applied Program tree. Every active
  component of the Deployment must be enabled. When some are not, Execute
  offers **Activate and execute**: it prepares and enables every active
  component, then executes the Program once.
- **Run** starts motion. A Program starts held: each motion group shows **Run**
  in the Motion panel, and nothing moves until it is pressed. **Stop** stops a
  group, **Resume** continues it, the **All groups** row's **Stop** stops all
  motion in the cell, **Halt** also pauses the tree, and **Reset** ends the
  run ([Stop, Stop all and Halt](#stop-stop-all-and-halt)).
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

### Stop, Stop all and Halt

ManyForge has three Stop levels. They are operational software stops, not
safety functions: the cell's emergency stops belong to the integrator's
safety controller.

- **Stop** (a group's **Stop** in the Program panel) stops the Program motion
  of the selected groups. A manual move in progress keeps moving, and the
  Program tree keeps running; its motion in the stopped groups waits.
  **Resume** on a group continues it.
- **Stop all** (the **Stop** of the **All groups** row) stops all motion in
  the cell: every group is stopped and every manual operation stops. The
  Program tree keeps running, and each group waits for its Resume. Stop all
  also works with no Program loaded, and a new manual move is allowed right
  after it.
- **Halt** is Stop all, and in addition pauses the Program tree and blocks
  new manual moves until **Execute** or **Reset**. The **Halt** button sends
  Stop all at once, then asks whether to halt the Program (*Confirm before
  halting the Program*, under Layout and confirmations in the panel
  settings, on by default; with it off, Halt follows at once). **Cancel**
  leaves every group stopped, ready for Resume. After **Execute** the tree
  continues, but motion stays stopped until **Run** or **Resume** on each
  group. With no Program run, Halt stops everything and blocks nothing
  afterwards, so it asks nothing.

The **Stop** of the **All groups** row is enabled while something can move: a
manual move, or Program motion that is running, starting or stopping. **Halt**
is enabled then too, and also while a Program run is active, even with all
its motion stopped. With nothing to stop, both are disabled and their tooltip
says so (*Nothing is moving*, *Nothing is running*). While the live status feed
is not connected and current, both stay enabled, since what the panel shows
may be out of date.

Output writes (**Set output** and a pneumatic gripper's valve writes), Scene
nodes and **Set TCP** are atomic: a Stop never cuts them off. The node waits
for its answer while the Program tree keeps ticking. If a Stop that holds the
node, or a Halt, comes before the request was sent, it is not sent: the node
stays running where it is and sends it once it runs again (after Run, Resume
or Execute). If the request was already sent, it completes: the node returns
its real result (success, or failure with the device's code), and the Halt,
or the end of the run on Reset, takes effect only after that answer.

A Stop is never refused. A Stop that names an unknown group, operation,
component or resource, or whose request is invalid, is carried out as Stop
all; an invalid Halt is still a Halt. Resume, Run, and Execute after a Halt
are refused with `program_selection_manual_operation_active` while a manual
move still runs on one of the robots they would release.

If a Stop cannot confirm that a robot stopped (its last Stop stage failed),
ManyForge raises a Halt by itself. That robot's component shows **Stop not
confirmed** with activity unknown, and the cell stays latched, until its
controller reports it idle again. Check the robot.

A Program can stop the cell itself with the **Cell Stop** node
(`kernel.cell_stop`): every operation in the cell stops, manual ones
included, and the Program is not held. The node succeeds when that Stop is
complete, and fails with `stop_not_contained` when a robot's Stop was not
contained.

No request from the Manager to the execution host has a time limit: its
launch, its status, the Apply fence, cell admission, Host Apply, projection
requests and the liveness reports all wait for the host's answer however long
it takes. A slow answer halts nothing and does not make the execution host
count as faulted. While one request waits, other requests to the host answer
`execution_host_late` at once (refused before any effect; try again), and
while it prepares projections, reads its component processes during an Apply
or rebuilds a projection they answer `runtime_unit_launch_in_progress`. The
Manager's own loop never waits on the host: it reads each status answer at a
later tick. A request still unanswered at the Manager's next tick is logged as
`execution_host_late_reply`, and its answer as
`execution_host_late_reply_settled`. Stops never wait on these requests. A
wait ends early only when the host exits (`execution_host_exited`; ManyForge
stops the application) or ManyForge stops (`application_stopping`). An Apply has no overall time
limit either; while one runs, a new Apply, New or a failed-Apply release
answers `apply_busy` and nothing is done.

### Components and Deployment

The center-top sections are Program, Scene, Components and Deployment.

**Components** shows the cell. On the left, the list: every component of the
Deployment Draft and of Manager Applied, with its kind and model, a tool's
mount (`lite_gripper → lite6_robot.flange`) and its state, or Added in Draft /
Removed in Draft; a machine's members sit under it. Above the list,
**Prepare all** and **Enable all**; below it, **Add component**. On the right,
the selected component's page:

- what it is: profile, machine and member role, mount or mounted tool and
  Deployment status, with **Open in Robot Control** (jogging and gripper
  commands stay in the viewer's Robot panel) and, for an unapplied change,
  **Review in Deployment Changes**;
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
- **Program binding**: each Program role (satisfied, unbound or
  incompatible) and the Program Binding.
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

**AI assistant.** The chat is a component: **Add component > AI assistant**
adds it (one per cell; pick its **AI model** and **Assistant profile** on its
card). It needs an AI model component that names your OpenAI-compatible model
server (Base URL, Model, optional token variable `MANYFORGE_AI_*`, exported
before `start`). Prepare starts its host, Enable starts the assistant bridge on
a loopback port the host picks, Disable stops it; the workbench starts no
bridge itself. The Composer AI tab works while the assistant and its AI model
are enabled and no Program run exists. Edits from the AI tab land in the
Program Draft; Apply or Discard them. Setup, model-server examples and failure
codes: [assistant cookbook](ASSISTANT_E2E_COOKBOOK.md).

Scripts and assistants use the same routes: `GET
/api/deployment/component-catalog` lists the entries (MCP
`manyforge_deployment` action `component_catalog`); `POST
/api/deployment/draft/components` adds `components` and `attachments` rows in
one edit (action `add_components`); `POST /api/deployment/draft/mutations`
changes or removes existing rows (action `mutate`); `POST
/api/deployment/preview` returns the diff and `POST /api/deployment/apply`
applies it. `POST /api/scenario/load` and `POST /api/scenario/new` are Load and
New (MCP `manyforge_scenario`). Apply, Load and New answer when they are
accepted; their outcome is the row with their `request_key` in `apply_commands`
of `GET /api/deployment/workspace`. `GET /api/components` and `POST
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
| `start` says `Development image is unavailable: manyforge-ros-lyrical-full` (exit 69) | No local image yet: run `./scripts/compose-dev.sh pull` or `build` ([Install](#install)). |
| `pull` fails (`manifest unknown`, `denied`, or a network error) | No image is published for the version in `VERSION` (a checkout that is not a release), or `ghcr.io` is unreachable. Check `cat VERSION` and the network, or use `./scripts/compose-dev.sh build` ([Fall back](#fall-back)). |
| The first start on a new image compiles everything again | Expected: each image has its own build cache under `.manyforge-dev/images/<image id>/` ([Update](#update-to-a-new-version)). |
| Start fails with `command failed with exit …: npm ci` | The first start on an image downloads the frontend's npm packages: check the network, then start again. |
| Operator-mode start refused with `built Composer frontend is not a regular file: …/index.html` | The frontend build is missing: run the build step (`./scripts/compose-dev.sh build-native` in operator mode, which the operator launcher runs itself), or pass a built frontend with `--composer-static-dir` ([operator runbook](MANYFORGE_OPERATOR_RUNBOOK.md)). |
| Site settings refused at start | See [Site file errors](TROUBLESHOOTING.md#site-file-errors). |
| Project source missing, invalid or changed | Preserve the diagnostic, inspect the named project/source path, and review changes before retrying. Do not bypass identity checks or delete caches blindly. |
| Port occupied | Identify the listener on 3000/9010/9011/9020; do not kill an unrelated process. |
| Different or unhealthy workbench already running | Read status/logs first. Stop only with authorization before switching scenarios; do not spawn another stack to hide the failure. |
| Slow first start | Inspect build output. Lower `MANYFORGE_DEV_BUILD_JOBS` if compilation exhausts memory; avoid repeated startup attempts. |
| `status` says stopped after a failed start | The launcher removed the container it created, with its logs. Read the start output you kept (`tee`), or start again with its output captured. |
| `Scenario project is outside allowed roots` (exit 69) | Pass a project folder inside `examples/`, the only root startup accepts; a scratch folder such as `.manyforge-dev/` is refused. |
| Start refuses with `… node environment manifest is stale` | A generated source is out of date after a catalog or covered source change. Run `python3 scripts/regen.py --update` in the workbench image, review the diff, then `python3 scripts/regen.py --check` ([Identities and pins](../reference/IDENTITY_AND_PINNING.md)). |
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
installer or packaged ManyForge release: the published image holds ROS and
dependencies only, and the workbench builds and runs ManyForge from source.
