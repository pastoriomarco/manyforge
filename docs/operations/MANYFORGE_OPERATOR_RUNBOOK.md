# ManyForge operator runbook

> **Historical operator reference:** the `native` CLI profile name and dated
> O1 records below are retained as restoration inputs (DX-753); the native
> simulator executable is removed. Current ROS development uses
> [`DEVELOPMENT_WORKBENCH.md`](DEVELOPMENT_WORKBENCH.md). Workbench empty-ready
> and mock Phase A `qualified-live02` do not qualify these installed-launcher
> commands, the full Compose wrapper, an observed GUI journey, Isaac or hardware.

## 1. Preconditions and identity

Run the operator on the selected ManyForge runtime host. Orin is the default
native runtime/UI host; the source checkout, installed dependencies, manager
executable, catalogs, and all state paths below must be on that host. Use
absolute paths. The source must be clean: the launcher collects
`manyforge.source_identity.v1` and rejects a dirty checkout.

The exact clean source, package, compiler, and environment identities are
source-matched inputs and must be copied from the dated O1 evidence for the
commit being qualified. Do not reuse the historical values from an older
runbook revision or invent placeholders for a live launch.

| Input | Value |
|---|---|
| ManyForge commit/tree/fingerprint | See the exact dated O1 evidence for the qualified freeze |
| Package/compiler/environment digests | See the source-matched package record named by that evidence |

The three artifact identity inputs are mandatory and must come from the
source-matched build/package record; do not invent or reuse values:
`package_aggregate_sha256`, `compiler_build_manifest_sha256`, and
`node_environment_sha256`. O1.3 native records a `mounted_install`, not a
container image, so no container image ID is supplied to this native command.
The split Isaac profile also records its exact full ROS container image and
container ID alongside the mounted remote package and local world. Its exact
source/package/image/world identity must be published in dated qualification
evidence before live use.

### Prepare the immutable package root

Prepare the package root from the same clean source and pinned container used
for the selected build. The maintained all-row production materializer is the
only preparation path: it requires all three prepared model families and
validates their semantic runtime contracts. Runtime Stop behavior remains
covered by the normal component and integration test suites; standard package
creation does not consume test reports or executable hashes.

```bash
MANYFORGE_ROOT=/absolute/path/to/manyforge
PACKAGE_ROOT=/absolute/path/to/new-empty-package-root
MATERIALIZER=/absolute/path/to/manyforge_maintained_deployment_occurrences
PREPARED_LITE6=/absolute/path/to/lite6_lite_gripper_structural
PREPARED_UR5=/absolute/path/to/ur5_roboplan_structural
PREPARED_UR10E=/absolute/path/to/ur10e_robotiq_2f_140_structural
if [ -e "$PACKAGE_ROOT" ]; then
  printf 'PACKAGE_ROOT must be a new destination: %s\n' "$PACKAGE_ROOT" >&2
  exit 1
fi
mkdir "$PACKAGE_ROOT" || exit 1

"$MATERIALIZER" --check \
  --workspace-root "$MANYFORGE_ROOT" --package-root "$PACKAGE_ROOT" \
  --prepared "lite6_lite_gripper=$PREPARED_LITE6" \
  --prepared "ur5_roboplan=$PREPARED_UR5" \
  --prepared "ur10e_robotiq_2f_140=$PREPARED_UR10E"
```

The command must complete successfully before the root is usable. Discard a
failed or partial root. Capture the package-root aggregate only after
successful materialization:

```bash
PACKAGE_SHA256="$(
  python3 -m manyforge_operator.package_aggregate --package-root "$PACKAGE_ROOT"
)" || exit 1
```

The command emits one lowercase SHA-256 digest. The native `start` below
independently recomputes that aggregate and rejects an unavailable, changed,
or mismatched root before state creation, port probes, or process effects.
The digest is provenance/integrity evidence only; it does not alter component
compatibility or admission semantics. Independently reload the successful
root through Manager `start`, then keep that root read-only for the entire
Manager generation. Do not select a root while materialization is running,
mutate it after reload, or add a second generator mode.

Before starting, reserve the frozen ports: Composer `9000`, Headless
loopback `9010`, and Program runner loopback `9011`. Keep the runtime host
reachable from the laptop on a trusted LAN. Composer has no bearer-token
semantics in this boundary; do not expose it beyond that trusted network.

## 2. Historical topology and commands

The operator owns four processes on one runtime host: **Manager**, **Headless
API**, **Program runner**, and **Composer**. Manager owns runtime convergence
and, from the exact admitted retained bundle, owns the ROS execution-host
child and its private runtime channel. The operator does not fork or adopt
that child separately. Headless and Program runner expose owner facts and
intents; Composer is a stateless client. Composer binds `0.0.0.0:9000`;
Headless and Program runner bind loopback only.

Set these to the actual source-matched installation. Every path is required
by `start`; the hash values must be full lowercase SHA-256 strings. `MANAGER`
must be colocated with the source-matched `manyforge_ros2_control_execution_host`
and `manyforge_roboplan_candidate_worker` targets because Manager resolves
those owned runtime targets from its executable directory. The admission
filename is package-relative and selects the exact idle cell; it must exist
under `PACKAGE_ROOT`.

```bash
MANYFORGE_ROOT=/absolute/path/to/manyforge
MANYFORGE_COMMIT=<qualified-manyforge-commit>
MANYFORGE_TREE=<qualified-manyforge-tree>
SOURCE_FINGERPRINT=<qualified-source-fingerprint>
STATE_ROOT=/absolute/path/to/operator-state
INSTANCE_ID=native
ADVERTISED_HOST=orion-hostname-or-address
CELL_ID=lite6_pick_place_cell
ADMISSION_FILENAME=canonical-empty-admission.lite6_pick_place_cell.json
MANAGER=/absolute/path/to/manyforge-manager
PACKAGE_ROOT=/absolute/path/to/package-root
NODE_CATALOG=/absolute/path/to/node_catalog.yaml
ALARM_CATALOG=/absolute/path/to/alarm_catalog.yaml
COMPOSER_STATIC_DIR=/absolute/path/to/composer-dist
PACKAGE_SHA256=<package-aggregate-sha256>
COMPILER_SHA256=<compiler-build-manifest-sha256>
NODE_ENV_SHA256=<node-environment-sha256>
```

The exact current start shape is:

```bash
python3 -m manyforge_operator \
  --state-root "$STATE_ROOT" --instance-id "$INSTANCE_ID" --json \
  start --profile native \
  --source-root "$MANYFORGE_ROOT" --manager-executable "$MANAGER" \
  --install-root "$PACKAGE_ROOT" --node-catalog-path "$NODE_CATALOG" \
  --alarm-catalog-path "$ALARM_CATALOG" \
  --composer-static-dir "$COMPOSER_STATIC_DIR" \
  --canonical-empty-admission-filename "$ADMISSION_FILENAME" \
  --cell-id "$CELL_ID" \
  --package-aggregate-sha256 "$PACKAGE_SHA256" \
  --compiler-build-manifest-sha256 "$COMPILER_SHA256" \
  --node-environment-sha256 "$NODE_ENV_SHA256" \
  --advertised-host "$ADVERTISED_HOST" \
  --expected-source-fingerprint "$SOURCE_FINGERPRINT" \
  --expected-head-commit "$MANYFORGE_COMMIT" \
  --expected-head-tree "$MANYFORGE_TREE" \
  --readiness-timeout-s 60 --poll-interval-s 0.25 \
  --reconciliation-period-ms 500 --manager-timeout-s 60
```

`--readiness-timeout-s`, `--poll-interval-s`, and
`--reconciliation-period-ms` are shown with their current defaults.
Manager verifies immutable bundle/product inputs once per Manager package
generation and reuses those typed values. The 500 ms reconciliation loop
therefore checks only changing process, liveness, readiness, graph and outcome
facts; it does not repeatedly hash package files. An actual native launch still
opens and digest-checks its protected executable. Replacing the package remains
a complete Manager-stack restart, which drops the verified package view.
The operator creates and validates one strict cell layout at
`$STATE_ROOT/$INSTANCE_ID`. Durable Manager, execution, Program-selection,
Program-runner, scene, historian, Composer-authoring, and operator state lives
under its `state/` subtree. Sockets, locks, native planning stages, and other
rebuildable process material live under its separate `run/` subtree. Artifact
and projection roots are also launcher-owned. These are deliberately not
independent CLI inputs: a marked layout is validation-only on reopen and the
launcher will not repair, merge, or reinterpret caller-assembled roots.
`start` waits for owner-issued readiness and leaves the stack idle. It does
not select a scenario, Apply a graph, or Run a Program. A compatible running
instance returns `already_running`; an incompatible instance is not adopted.

Use the same state root and instance ID for the exact status/stop shapes:

```bash
python3 -m manyforge_operator \
  --state-root "$STATE_ROOT" --instance-id "$INSTANCE_ID" --json status

python3 -m manyforge_operator \
  --state-root "$STATE_ROOT" --instance-id "$INSTANCE_ID" --json stop
```

Open the stable URL from the runtime host or a trusted-LAN laptop browser:
`http://<advertised-host>:9000/`. The laptop is only a client; it does not
start the runtime. `/api/health`, `/api/manager/status`, `/api/runtime`, and
`/openapi.json` are useful inspection surfaces. The generated
[`REST_API.md`](../reference/REST_API.md) is the route contract.

## 3. Deliberate Composer operation

### Saved Program authoring

Native `start` deterministically creates one instance-owned writable Program
workspace:

```text
$STATE_ROOT/$INSTANCE_ID/program-workspace
```

Composer alone receives that directory in addition to the immutable
`PACKAGE_ROOT`. Manager, Headless, and Program runner retain package-only
filesystem access. The workspace path is also recorded in the launch record's
internal mechanical `paths` metadata; it does not add a launcher option or
change the public status DTO.

To derive an editable Saved Program from maintained package content:

1. Open the packaged Program as a Draft in Composer.
2. Choose **Save as** and select a new `*.program.yaml` path under the
   instance `program-workspace`.
3. Continue editing and use **Save** for that workspace copy.
4. Stop and restart the native stack with the same `STATE_ROOT` and
   `INSTANCE_ID`, then reopen the Saved Program from `program-workspace`.

The package is immutable reference content and is never a Save or Save As
destination. Stop, failed child recovery, operator CLI restart, and complete
stack restart perform process cleanup only and preserve workspace authoring
content. Back up or remove that content only through a separate deliberate
operator action; the launcher never deletes or replaces the workspace.

1. Start native and wait for `ready`; inspect health, runtime, graph, alarms,
   and the current desired/active revisions.
2. Select a maintained single-cell input or compose the current desired graph.
   Use Composer's Preview/Validate and inspect the diff/revision before any
   mutation. Resolve validation or admission failures; do not treat a preview
   as execution.
3. Explicitly choose **Apply**. Confirm the owner-issued Manager and Applied
   status and the resulting revision.
4. Only after Apply is confirmed, explicitly choose **Program Run**. Follow
   the owner-issued run status through its terminal state; use the explicit
   Program Stop or execution Stop when required. A browser reload is safe:
   Composer does not own or cancel the run.

Apply and Run are separate operator decisions. Composer start never performs
either action, and Composer never becomes the authority for runtime, scene,
execution, lifecycle, alarm, or Stop truth. The API Stop surface is not a
machinery E-stop; use the independent safety system/E-stop for a hazardous
physical cell. Real-robot, tool-attachment, repeated-instance, and mixed-cell
lanes remain deferred (I10/I11/I12/R).

## 4. Truthful status and recovery

The shown `status` invocation with global `--json` reports the persisted
launch instance and owner readiness.
The only instance/readiness states are:

| State | Meaning |
|---|---|
| `unavailable` | An owner/readiness observation cannot be obtained. |
| `starting` | Owned processes are being brought up and readiness is pending. |
| `ready` | All three owner health checks report ready. |
| `degraded` | A process, port, or owner health/readiness observation is missing or failed. |
| `stopping` | Bounded owner-safe shutdown is in progress. |
| `stopped` | Absence of the recorded owned processes was observed. |

Process or port presence alone is never semantic readiness. A missing state
record returns `instance_unavailable`; repeat `stop` is harmless when a
persisted instance is already `stopped`. Start failure records compact
failure reasons. In particular:

- `source_dirty` or `source_identity_mismatch`: restore the exact clean source
  anchor and matching expected identity; do not bypass the check.
- `port_occupied`: free the named `9000`, `9010`, or `9011` conflict without
  killing an unrelated process. Ports are frozen; do not silently remap them.
- `stale_launch_record` or `incompatible_owner`: do not adopt the processes;
  inspect the recorded instance locally and use its matching identity.
- Loss of the Manager-owned managed child is recovered by Manager's
  observe-first reconciliation. It must receive a fresh runtime epoch and a
  fresh selected/observed offer-digest correlation before commands reopen; it
  never adopts an arbitrary process.
- Loss of Manager, Headless, Program runner, or Composer is outer-stack loss,
  not child recovery. Use the supported complete-stack `stop` followed by
  `start` with the same desired configuration and immutable package root.
- Replacing the package root requires complete-stack `stop`/`start`. Manager
  performs no live mixed-package upgrade and has no component-level version
  compatibility path.
- Normal Manager shutdown stops launch groups in parallel and waits for actual
  host exit. Each group's existing SIGINT grace and KILL escalation remains;
  worker, projection and owner destruction has no automatic total kill timer.
  Shutdown logs name the current phase and actual completed milestones.
  If progress stalls, the log says `shutdown stuck at <phase>; to force:
  manyforge-operator stop`. Run the existing `manyforge-operator stop` with the
  same `--state-root` and `--instance-id` shown above to force the recorded
  instance's bounded TERM/wait/KILL cleanup. This is an explicit operator
  action; a responsive status reply alone does not prove teardown progress.
- Stop asks Manager for semantic shutdown, then performs bounded authenticated
  process cleanup. Only the recorded launch instance may be terminated; a
  deadline failure is `cleanup_deadline_exceeded`, not success.
- If Orin or the runtime host is unreachable, a laptop browser cannot recover
  it. Native status/stop must be run on the runtime host; split Isaac status/
  stop reports the remote-unavailable condition when it cannot prove a stopped
  record absent. Restore the host, then use the matching `status`/`stop`/`start`
  flow. Do not use broad process-name kills or SSH recipes.

## 5. Split Isaac topology and other unavailable lanes

The Isaac profile keeps the outer lifecycle on the laptop and starts one
source-matched full ROS image container on the remote ROS machine through the
existing `scripts/compose-dev.sh` / `scripts/dev-workbench.py` route. Manager,
ROS execution host and cell pose observer share that container; components
remain managed processes. ROS is not installed or sourced on either host.
Isaac Sim stays on the laptop, using its bundled ROS environment and existing
`isaacsim.ros2.bridge` / `isaacsim.ros2.sim_control` extensions.

The container retains Headless on port9010 and Runner on9011 (loopback), and
Composer with its built static frontend on port9000 (all interfaces). It does
not start Vite or run `npm ci`. Normal developer Workbench defaults are
unchanged. G READY requires its authenticated Sim, fresh Manager-ready status
and actual service health. It does not certify Apply/CCLC convergence, fresh
poses, Program execution or motion permission. Build time and readiness time
are reported separately; this source wiring is not live qualification.

When a site file is configured, G forwards its explicit path and the verified
absolute observer executable from the same built overlay to Manager. The
Manager-owned check/observer path uses the retained site snapshot before world
effects; it creates no host ROS dependency or second observer owner.

The temporary Isaac qualification hold records Jazzy setup paths as compatibility
inputs; neither host sources these paths. ROS runs inside the qualified image.
`HELD_ISAAC_ROS_IMAGE_ID` must identify the exact qualified image on the remote
host; the common Lyrical image is not a substitute. The current Orin inventory
has no qualified arm64 held Jazzy full image (the preserved laptop image is
amd64), so this recipe is unavailable there until one is
explicitly supplied and qualified.

Use the same global state root/instance ID and provide every source-matched
remote path and digest. The command shape is:

```bash
python3 -m manyforge_operator \
  --state-root "$STATE_ROOT" --instance-id isaac --json \
  start --profile isaac --source-root "$MANYFORGE_ROOT" \
  --isaac-executable "${HOME}/isaac-sim/isaac-sim.sh" \
  --world-path "${HOME}/workspaces/isaac_ros-dev/src/isaac_sim_custom_examples/Scenario/IsaacManipulator/UniversalRobots/isaac_manipulator_scene_light.usda" \
  --local-ros-setup-script /opt/ros/jazzy/setup.bash \
  --local-ros-environment-script "$MANYFORGE_ROOT/config/ros/isaac_i9.env.sh" \
  --remote-target tndlux@192.168.1.68 \
  --remote-source-root /mnt/nova_ssd/codex-manyforge/runners/manyforge \
  --remote-container-image-id "$HELD_ISAAC_ROS_IMAGE_ID" \
  --remote-container-name manyforge-isaac-runtime \
  --remote-cache-root /mnt/nova_ssd/codex-manyforge/cache/isaac-runtime \
  --remote-runtime-root /mnt/nova_ssd/codex-manyforge/state/isaac-runtime \
  --remote-evidence-root /mnt/nova_ssd/codex-manyforge/evidence/isaac-runtime \
  --remote-manager-executable /mnt/nova_ssd/codex-manyforge/builds/current/package/bin/manyforge_manager_daemon \
  --remote-install-root /mnt/nova_ssd/codex-manyforge/builds/current/package \
  --remote-node-catalog-path /mnt/nova_ssd/codex-manyforge/builds/current/package/share/node_catalog.yaml \
  --remote-alarm-catalog-path /mnt/nova_ssd/codex-manyforge/builds/current/package/share/alarm_catalog.yaml \
  --remote-state-root /mnt/nova_ssd/codex-manyforge/runners/cells \
  --remote-bridge-state-root /mnt/nova_ssd/codex-manyforge/runners/isaac-bridge \
  --remote-simulation-control-config /mnt/nova_ssd/codex-manyforge/runners/manyforge/manyforge_ros_bringup/config/isaac_program_simulation_site.json \
  --remote-ros-setup-script /opt/ros/jazzy/setup.bash \
  --remote-ros-environment-script /mnt/nova_ssd/codex-manyforge/runners/manyforge/config/ros/isaac_i9.env.sh \
  --package-aggregate-sha256 "$PACKAGE_SHA256" \
  --compiler-build-manifest-sha256 "$COMPILER_SHA256" \
  --node-environment-sha256 "$NODE_ENV_SHA256" \
  --expected-source-fingerprint "$SOURCE_FINGERPRINT" \
  --expected-head-commit "$MANYFORGE_COMMIT" \
  --expected-head-tree "$MANYFORGE_TREE"
```

`HELD_ISAAC_ROS_IMAGE_ID` is the full immutable `sha256:<64 lowercase hex>` ID of
an already present, qualified full ROS image. G does not pull or build an image.
The qualified package/admission and catalogs are mounted read-only at their
explicit absolute paths. An optional `--remote-composer-static-dir` overrides
the image's existing built frontend. The older executable/state/setup path
arguments remain recorded compatibility inputs; the Workbench builds and stages
its resident runtime from the verified source and image. Host ROS setup paths
are not sourced. The topology inputs remain the existing explicit domain, RMW,
discovery range and static peers, forwarded into the same container. The
example's site file must be an actual configured source, not a placeholder.

The runtime root must be persistent absolute host storage, outside `/tmp`,
`/run` and tmpfs. Manager, Scene, cycle and incident journals survive container
removal and reboot. Build caches and existing Composer/Runner persistent values
are retained separately. Status and stop authenticate the original full
container ID, immutable image and launch labels; replacement names are not
ownership proof. A failed inspection or unreachable Docker/SSH endpoint is
unknown, not proof of absence.

G keeps Docker's `json-file` logs at50MiB ×3 and records the exact
`docker logs <full-container-id>` reference. Its plain stop and reuse of an
exited owned container first export through the existing `stop_with_evidence`
path into the one configured evidence root. Export failure retains the
container and its logs. Failed startup first leaves the exact container
available to G: if usable, G requests SimulationStop only for its authenticated
own Sim, then stops/exports/removes the container. If unusable or unknown it
reports that limitation; it does not create a temporary ROS client or claim
Sim Stop succeeded. Existing owned local Sim process cleanup remains bounded.

The global size/count policy for exported bundles, failed-export retained
containers, ROS home logs and per-launch records is still pending. Do not add
an automatic rotator or delete uncaptured evidence. Explicit `status` and
`stop` use the persisted profile and identity; Isaac never auto-plays, selects
a Scenario, Applies a graph or Runs a Program. Startup/cleanup failure and
host/Sim loss remain typed stopped/degraded outcomes. Restore an unavailable
host before reconciling the matching record; never use broad process kills,
an unowned Sim or historical launchers as a fallback.

The physical ROS/real-robot lane, assistant-provider bring-up, packaged
installation, historical aggregate launchers, and old Isaac recipes are not
current operator surfaces. In particular, never use or restore historical
`scripts/launch.sh` or `scripts/lib/isaac.sh`; they are forbidden fallback
entrypoints, not alternatives to `manyforge-operator`.
