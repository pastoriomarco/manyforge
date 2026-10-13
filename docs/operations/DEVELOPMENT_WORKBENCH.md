# Development workbench

Status: current development-only entrypoint. This is not an installed product
launcher and does not authorize physical execution.

The workbench starts the current frontend with Vite hot reload and an empty
simulated native backend. It may activate one exact Scenario project folder.
One command owns file placement, ports, process order, readiness checks,
optional project activation, diagnostics, and cleanup.
Browser mount, reload, and disconnect are read-only reconstruction events.

## Prerequisites

This public runbook is sufficient for operation; no private documentation
checkout is needed. The supported host path uses Linux host networking, Docker
Engine with Compose v2/BuildKit, Git and Python 3. Allow network access for pinned
dependency downloads and enough disk/RAM for the native build. This is source-based
workbench setup, not a packaged production installer.

Prepare only the required image once, from the repository root:

```bash
./scripts/setup-docker.sh --prepare-only
docker compose build dev
```

The launcher builds current native owners, the activation preparer and a small
canonical-empty bootstrap from this checkout. It does not import a maintained
package or installed Scenario catalog, and it does not run tests or proofs.
The setup script may recreate stale or modified dependency directories under
`.docker/`; preserve wanted local dependency edits before running it.
Selected projects are read through the ordinary Scenario-project loader and
validated before activation.

The workbench owns one stable internal runtime identity, independent of which
Scenario is selected. Portable `manyforge.deployment_intent.v2` documents do not
contain `cell_id`. Starting empty and starting with a project both establish
the same command-closed native host before ordinary Load/Apply. Loading another
Scenario changes its equipment, Scene and Program, not the runtime identity.
Multiple instances competing for the same equipment are unsupported.

### Development ROS control preparation

`start --ros2-control` is an explicit development preparation option for a
source-matched full ROS image selected through `MANYFORGE_DEV_IMAGE`. The
ordinary default remains the native workbench. This option builds the native
host, ROS execution host and shared candidate worker in one ROS-enabled build
graph, stages them for the existing Manager installed-target protocol, and
connects the Program Runner's existing simulation-control port using
`manyforge_ros_bringup/config/isaac_program_simulation_site.json`.
The authored compiler accepts the existing UR10 robot/gripper ROS profile
through ordinary Scenario Load preparation, carries the required authored
Deployment TCP speed maximum into ROS runtime creation, and corrects native-only
configuration access in that host. The Deployment owner refreshed the affected
artifacts. Focused Orin Docker checks passed all nine preparation cases, four
retained-configuration cases and 32 workbench/launcher tests; both execution
hosts and the shared candidate worker built, and the owner freshness check
passed. The isolated [offline diagnostic receipt](../../../runner-evidence/orin/freeze-8/final-receipt.json)
records those results and their frozen inputs. A later isolated diagnostic
route used the existing preparer to derive the ROS canonical-empty predecessor
from the intended first Scenario, preserving its exact runtime products and
normal Manager fencing. The [successor receipt](../../../runner-evidence/orin/freeze-13/liveness-successor-receipt.json)
records ROS predecessor-continuity and existing liveness-policy repairs with
seven focused checks and the affected executable builds passing.

The authorized live UR10 check passed typed playback/readback and the strict
existing-world barrier without loading another world. Passive joint observations
naturally aligned with the authored seed while the command gate remained closed.
Normal authored Load was accepted, but Apply ended `unknown_effect` with its
fence held and the previous empty deployment still active: the ROS host explicitly
refuses a nonempty component delta until it can produce a qualified lifecycle
provider receipt (`ros2_component_delta_not_qualified`). The
[live receipt](../../../runner-evidence/live/attempt-2-receipt.json) records the
paused simulator, closed gate and retained evidence. That candidate was shut down
normally and its unknown-effect Apply was not retried. The reviewed first-install
successor produced the existing owner receipts, and a fresh normal Load/Apply
succeeded with the aggregate active, the Apply committed and its fence released.
The [Step 1 receipt](../../../runner-evidence/live/attempt-3/step1-live-receipt.json)
records advancing commanded idle observations and a closed command gate without
automatic home motion. A bounded read-only arm preview was refused with
`scene_changed_during_validation`; the logical Scene/collision projection join
remains unresolved. No motion or Stop qualification is claimed. Gripper-first
operation remains deferred to the planned arm/tool separation.
The subsequent bounded mesh-resolver correction passed 21 focused Docker tests;
all five active-scene meshes returned HTTP 200 metadata and valid GLB geometry
after a fresh normal Load/Apply. The [mesh repair receipt](../../../runner-evidence/live/attempt-4/mesh-repair-receipt.json)
records the unchanged access roots, healthy owners and preserved Isaac world.
The ordinary ROS option now requires an explicit intended first Scenario and
uses the production preparer to derive its exact ROS canonical-empty predecessor
before Manager starts. `--bootstrap-project` selects that Scenario while opening
an empty workspace; complete the separately operated simulator/controller
prerequisites, then use ordinary Composer Load/Apply for that Scenario.
`--project` selects the same derivation and also requests the existing startup
Load/Apply, so the simulator/controller must already be ready. Managed ROS
installation seeds the declared commanded state and checks controller readiness
with the shared command gate closed; accepted tracked arm execution later opens
that gate. These options are mutually exclusive. Native or
mixed execution selections, incomplete/invalid artifacts, changes during
preparation and missing ROS selection are refused. This startup route is
development preparation; the diagnostic receipts are not installed-launcher or
completed live qualification.
Mixed native/ROS compositions and other ROS profiles remain unsupported.

For an already prepared full ROS development image:

```bash
MANYFORGE_DEV_IMAGE=<source-matched-full-ros-image> \
  ./scripts/compose-dev.sh start --ros2-control \
    --bootstrap-project examples/isaac-ur10e-light-scene.scenario
```

The image entrypoint must supply its ROS and Python environment. Startup builds
the mechanical ROS overlay and imports its setup into the service environment;
Manager children inherit that environment. Explicitly exported standard DDS
inputs, including `ROS_DOMAIN_ID`, `RMW_IMPLEMENTATION`,
`ROS_AUTOMATIC_DISCOVERY_RANGE` and `ROS_STATIC_PEERS`, pass into this container
unchanged. Configure the selected topology before starting it. Exported DDS
profile-file paths are preserved too; each referenced XML or other
configuration file must exist at that path inside the container. A host-only
absolute path does not become container-visible by exporting it.

The ROS option runs with the launching host user's UID/GID and uses the
image-keyed `/runner/home` and `/runner/build` bind mounts for writable home
and build caches. The repository, caches and persistent state directories must
be writable by that user before startup. This supports hosts whose UID differs
from the full ROS image's development user without changing the default native
container identity.

This option starts neither Isaac Sim nor its mechanical controller/bridge launch, and
does not choose peers or load a simulator world. Those prerequisites remain
separate, explicitly operated processes. A running native workbench cannot
be silently reused for a ROS request; stop it before changing this option.

The retained `examples/isaac-ur10e-light-scene.scenario` is an aggregate
robot/gripper development input. Its explicit 0.25 m/s authored TCP ceiling is
demo policy shared with the native UR10e example, never a default for a missing
limit. Live Isaac qualification remains unavailable;
this preparation option does not qualify Load, motion, Stop, Reset or repeated
Runs. Arm-only profiles and installation-matched observation configuration
remain additional integration work. The observer site's `cell_id` must match
the workbench installation, `c1_build_authority_cell`, and its Manager run root
must point at this workbench's runtime state.

Existing v1 authored Deployment files must be migrated explicitly: change their
intent version to v2 and remove the authored `cell_id`. Do not remove cell IDs
from compiled Deployment artifacts or rename existing runtime journals.

## Start, inspect, and stop

### Everyday workflow

Run every command from the repository root.

1. **Start** the workbench with one repository Scenario project folder, or
   with no project for an empty authoring workspace:

   ```bash
   ./scripts/compose-dev.sh start --project examples/ur5e_lite6_wall_workbench.scenario
   ./scripts/compose-dev.sh start
   ```

   `examples/ur5e_lite6_wall_workbench.scenario` is the editable everyday
   UR5e + Lite6 Scenario; edit and Save it in Composer. It has its own
   Scenario ID, so its stored persistent values and Applies are separate from
   other Scenarios. Do not edit or Save
   `examples/ur5e_lite6_wall_grouped.scenario` in Composer: it is a generated
   qualification fixture that tests check byte for byte. Change it only through
   its generator, `scripts/qualification/generate_ui6i_dual_robot_scenario.py
   --update` (with the built Deployment and Program revision tools).

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
the host; keeping those records does not resume motion or a Program. The
selected saved Scenario is loaded through the ordinary startup path.

During every start the log shows two error-looking lines before the native
execution host is up. They are expected and stop once the start reports ready:

- `Execution event pump unavailable: … execution-command.sock … No such file or directory`
- `Execution event pump unavailable: … live logical-scene projection is unavailable`

### Reference

Frontend source edits update through HMR. Backend
startup incrementally builds the current `manyforge_manager_daemon`,
`manyforge_headless_py`, `manyforge_native_execution_host`,
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
Unidentified historical containers and legacy unmounted containers whose retry
fails stay protected: the warning names them and asks for manual export or
explicit removal after investigation. Exit 69 after a capture failure means
the container stopped, capture failed and the stopped container was retained;
it does not mean shutdown failed. The launcher reuses the receipt-proven mounted
partial in place. Do not store user files in ManyForge runtime folders: extra
files and directories inside positively owned captures or artifact epochs may
be deleted with an eligible unit, or removed when a capture is replaced.
Links inside an owned unit are unlinked without following their targets. A
pre-existing mount point inside a unit protects that unit and produces a warning.
Concurrent privileged mount changes are outside the supported operator scope. Unknown ownership and multiple matching historical partials
retain their refusal and protection. Back up wanted evidence elsewhere.

For transitional containers without the persistent-root mount, removal still
uses the former full-state export under `MANYFORGE_DEV_EVIDENCE_ROOT` (default
`.manyforge-dev/evidence/`), excluding reproducible installed binaries, model
copies, prepared activations and unpacked planning meshes. Mounted containers
always use their actual runtime root's evidence directory. A legacy
replacement refuses a nonempty destination root; ordinary mounted replacements
reuse their existing root. There is no automatic migration. Legacy retries
replace the same `.legacy-<container-id>.partial` folder with a matching
receipt instead of accumulating full-state partial copies. Unknown historical
partials remain protected. The legacy export path can be removed once an inventory confirms that no unmounted containers remain.

Q99 runs one bounded retention pass before a new container starts and after a
confirmed stop, after checking that no running, paused or restarting ManyForge
container uses that root. Serialize operations sharing an explicit root; the
mount inspection is a point-in-time check. Selected roots may have symlinked
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

Only older completed console captures, superseded generated controller-launch
artifact epochs, and older owned host ROS log sessions are eligible. ROS host
logs use `execution-run/ros/<sequence>-<session-start-ns>`, stamped once before
the first workbench service starts under the existing state-root lock. The
highest sequence identifies the latest session independently of wall-clock
changes and is always protected; an older
session containing a file at or after that start time is also protected.
Legacy timestamp-only names and malformed or ambiguous names remain protected.
Latest-session discovery lists all top-level names; recursive accounting still
examines at most 1,000 candidate units per pass, with sequenced sessions ahead
of protected legacy names. A failed name listing protects
all observed ROS sessions, while a limited deep inventory does not stop
reclamation of independently qualified older sessions. The stable runtime-wide
`execution-run/ros-home` preserves spawner lock coordination across controllers
and epochs and is never rotated. Controller child logs retain their existing
per-artifact `ros` directory. Running a ROS host outside the supported workbench
requires explicit owned `ROS_HOME` and `ROS_LOG_DIR` environment paths; the host
provides no fallback path policy. Console chronology comes from
the generated UTC capture name; artifact chronology comes from the write time
of validated ManyForge component metadata markers. Extra files and directory
timestamp changes do not set this order; a marker write time is not an immutable
creation timestamp. At most eight eligible
deletion attempts run per pass; a failed deletion protects that folder for the
pass and cleanup continues with another eligible folder. The latest capture,
latest artifact epoch per bundle (including equal marker times), partial captures, unknown ownership, and
sibling roots are protected. Any open or unknown CCLC incident protects every artifact epoch, console capture and host ROS session; an
uncheckpointed or changed journal/WAL is unknown. Retention pins its read-only
protection inputs before inventory and checks that they still identify the
same journal before deletion. Sibling roots are rotated only on their own next
start/stop. Live journals, incident records, current publication/state, inputs
and build caches are never candidates. Directory inventories observe at most
1000 entries each. Overflow warns that usage is a measured lower bound and
unseen material is protected. Cleanup can consider independently eligible
observed units only when measured usage or the actual filesystem free-space
floor proves pressure, oldest observed first. Protected-only or unresolved
pressure reports a warning and stops cleanup; it never gates motion or
startup.

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
directory; the operator-managed path already requires an explicit persistent
directory. A start refuses a state root bound by another running ManyForge
container. Give concurrent container names separate roots. On an Orin, place
`MANYFORGE_DEV_CACHE_BASE` or the explicit runtime root under `/mnt/nova_ssd`,
so these files stay on the SSD rather than the eMMC home.

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

Apply through the private Manager socket does not prepare the program scene; use Headless.

Use Composer at `http://localhost:3000` for Program, Scene and Deployment work.
Read Runtime/history and component readiness before issuing commands. Apply
activates configuration; Save persists an artifact. Motion requires a separate
authorized command. Browser closure does not Stop the runtime, and an unconfirmed
viewer pose must not be treated as current measured state.

The ordinary authoring artifact is a Scenario project directory with fixed
members: `scenario.yaml`, `deployment.yaml`, `program.yaml` and
`assignment.yaml`, plus optional `sources/` and `assets/`. Open the project
directory to load those parts together. Program parameters always take their
Program defaults: a former project with `overrides.yaml` (or a
`manyforge.scenario.project.v1` manifest) is refused, not read; move any
needed value into the Program default and recreate the project. Save updates authored intent and source
references; Save As creates another project without copying referenced external
sources. Missing/invalid sources block preparation, while changed compatible
sources are reported for review rather than silently pinned to an old vendor
copy. Use Export when a portable project is required: it copies the admitted
source/asset closure and rewrites only the exported references. Do not use Save
As as an asset-collection operation.

UA2A qualifies this project and preparation workflow, not the new live
activation lifecycle. Until UA3–UA7 replace and qualify the current Manager/
Scene consumers, treat Apply/Revert behavior as development work in progress and
follow Runtime/history rather than assuming that successful preparation means a
completed live transition. Discard remains draft-only.

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

### Terminal Apply recovery (simulated workbench only)

Use this procedure only when Composer reports that a terminal Apply left the
runtime locked: an `unknown_effect` outcome, or a `failed` outcome other than
the exact pair `effect_disposition=known` and `failure_phase=accepted`. That
exact pair is a clean pre-effect refusal; correct its named cause and do not
restart the workbench merely because the Apply failed. The terminal record is
not admission readback: retry only when the current owner reports admission
open. If it remains closed or unavailable, inspect Runtime/history for the
current holder rather than assuming either successful settlement or a
catastrophic lock.

For a locked outcome, do not retry it or resume execution. From the repository
root, either close the failed runtime:

```bash
./scripts/compose-dev.sh stop --keep-evidence
```

Or preserve it and cold-reload the current Saved Scenario in a fresh container:

```bash
./scripts/compose-dev.sh stop --keep-evidence && \
  ./scripts/compose-dev.sh start --project 'path/to/saved.scenario'
```

The project argument must be a safely shell-quoted Scenario project directory
relative to the repository root; it must contain `scenario.yaml`. A Saved
Scenario outside the repository, or a Saved single-file `.scenario.yaml`/
`.scenario.yml` artifact, cannot be reloaded by this workbench command. Use
Close to preserve the evidence, then reopen such a Scenario through a
supported workflow. The page disconnects while the container is replaced and
may need a refresh; unsaved Draft changes are lost. Startup validates the
Saved Scenario, so invalid content leaves the runtime closed. A successful
reload does not start a Program or authorize motion automatically.

Ordinary Scenario Load remains a Manager Apply and does not restart the
workbench. These commands are a development-workbench fallback for a runtime
that deliberately retained its restrictions; the future installed GUI
affordance belongs to the installed lifecycle supervisor, not this script.

This procedure is only for the simulated development workbench. It is not a
containment or recovery procedure for physical equipment.

| Symptom | Action |
| --- | --- |
| Image missing | Prepare dependencies and build `dev` using the commands above. |
| Project source missing, invalid or changed | Preserve the diagnostic, inspect the named project/source path, and review changes before retrying. Do not bypass identity checks or delete caches blindly. |
| Port occupied | Identify the listener on 3000/9010/9011/9020; do not kill an unrelated process. |
| Different or unhealthy workbench already running | Read status/logs first. Stop only with authorization before switching scenarios; do not spawn another stack to hide the failure. |
| Slow first start | Inspect build output. Lower `MANYFORGE_DEV_BUILD_JOBS` if compilation exhausts memory; avoid repeated startup attempts. |
| `status` says stopped after a failed start | The launcher removed the container it created, with its logs. Read the start output you kept (`tee`), or start again with its output captured. |
| `Scenario project is outside allowed roots` (exit 69) | Pass a project folder inside `examples/`, the only root startup accepts; a scratch folder such as `.manyforge-dev/` is refused. |
| Start refuses with `… assistant instrument source is invalid or stale` or `… node environment manifest is stale` | A generated source is out of date after a catalog or covered source change. Regenerate with the owner tools, in order: `manyforge_behavior/scripts/generate_effective_node_catalog.py` (an intentional catalog change also updates the pinned digest in `manyforge_behavior/manyforge_behavior/catalog.py`), `manyforge_behavior/scripts/generate_node_environment_manifest.py`, then `manyforge_deployment/tools/generate_assistant_instrument_registry.py`. Run them in the workbench image; each has `--check`. |
| Error-looking `Execution event pump unavailable` lines during start | Expected before the native host is up; see the everyday workflow. Investigate only if they continue after the start reports ready. |
| Page stale or disconnected after a restart | Refresh the tab. |
| Stale viewer or failed command | Inspect Runtime/history and owner status. Do not infer rejection from a lost response or resend motion/Apply automatically. |

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
`physical_execution_authorized` remains false. The default prepares simulated
native execution; the explicit ROS option prepares development simulation
adapters and does not authorize physical equipment. Installed delivery and official
AMD64/ARM64 packages are separate release/distribution work.


## Operator-managed Isaac container

The split Isaac operator reuses this same launcher and resident Workbench in
one full ROS image on the remote ROS machine. It passes an explicit immutable
image ID, launch identity/digest, qualified package/admission, DDS topology,
site path and persistent runtime/evidence roots. This G startup mode keeps its
existing built-static Composer endpoint at port9000, without Vite/npm startup;
ordinary commands in this runbook keep their current defaults.

Manager, execution host and observer run together inside that container. The
observer executable is resolved and verified from the existing built overlay,
then forwarded to the Manager-owned site check and observer launch. No host
ROS setup or second component-container launcher is introduced. G readiness
reads fresh Manager and service health plus the owned Sim; it does not grant
motion permission or certify all configured components active.

G's runtime-state bind is persistent host storage (not `/tmp` or tmpfs), so
journals survive removal and reboot. Its plain stop, exited-container reuse
and owner-controlled failed-start cleanup all preserve runtime evidence and
Docker logs before removing the exact owned container. An export failure keeps
the container/logs. Failed startup returns exact metadata to G while retaining
the ROS client for authenticated own-Sim cleanup, rather than removing it in
the developer failed-start trap. Both paths use the bounded capture and Q99
retention policy above. Docker logs retain the independent existing
`max-size=50m`, `max-file=3` bound. See the
[operator runbook](MANYFORGE_OPERATOR_RUNBOOK.md#5-split-isaac-topology-and-other-unavailable-lanes)
for complete inputs, identity and qualification limits.
