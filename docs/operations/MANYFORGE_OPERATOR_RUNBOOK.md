# ManyForge operator runbook

> **Current boundary (O1 implementation snapshot):** the `native` and split
> `isaac` operator entrypoints, durable identities, recovery paths, and the
> Composer deployment-composition client are implemented and container-tested.
> Clean-source live qualification and `FV-O1` remain open; this document is
> not a live availability claim until the dated O1 evidence names the exact
> source/package/world identities and green runtime rows.

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
The split Isaac profile records a mounted remote install and a mounted local
world; its exact source/package/world identity must be published in the dated
O1 evidence before live use.

### Prepare the immutable package root

Prepare the package root from the same clean source and pinned container used
for the qualified build. The maintained all-row production materializer is the
only preparation path: it requires all three prepared model families and the
exact native and ROS 2 control Stop-qualification tuples. Copy every value
below from the source-matched build and dated qualification record; these
variables are not evidence placeholders.

```bash
MANYFORGE_ROOT=/absolute/path/to/manyforge
PACKAGE_ROOT=/absolute/path/to/new-empty-package-root
MATERIALIZER=/absolute/path/to/manyforge_maintained_deployment_occurrences
PREPARED_LITE6=/absolute/path/to/lite6_lite_gripper_structural
PREPARED_UR5=/absolute/path/to/ur5_roboplan_structural
PREPARED_UR10E=/absolute/path/to/ur10e_robotiq_2f_140_structural
NATIVE_STOP_IMPLEMENTATION_BUILD=/absolute/path/to/native-implementation-build.json
NATIVE_STOP_INSTALLED_TARGET=/absolute/path/to/native-installed-target
NATIVE_STOP_RUNNER_CONTRACT=/absolute/path/to/native-runner-contract.json
NATIVE_STOP_RUNNER=/absolute/path/to/native-stop-qualification-runner
NATIVE_STOP_IMPLEMENTATION_EVIDENCE=/absolute/path/to/native-implementation-evidence.json
NATIVE_STOP_SUBSTITUTION_EVIDENCE=/absolute/path/to/native-substitution-evidence.json
ROS_STOP_IMPLEMENTATION_BUILD=/absolute/path/to/ros-implementation-build.json
ROS_STOP_INSTALLED_TARGET=/absolute/path/to/ros-installed-target
ROS_STOP_RUNNER_CONTRACT=/absolute/path/to/ros-runner-contract.json
ROS_STOP_RUNNER=/absolute/path/to/ros-stop-qualification-runner
ROS_STOP_IMPLEMENTATION_EVIDENCE=/absolute/path/to/ros-implementation-evidence.json
if [ -e "$PACKAGE_ROOT" ]; then
  printf 'PACKAGE_ROOT must be a new destination: %s\n' "$PACKAGE_ROOT" >&2
  exit 1
fi
mkdir "$PACKAGE_ROOT" || exit 1

"$MATERIALIZER" --check \
  --workspace-root "$MANYFORGE_ROOT" --package-root "$PACKAGE_ROOT" \
  --prepared "lite6_lite_gripper=$PREPARED_LITE6" \
  --prepared "ur5_roboplan=$PREPARED_UR5" \
  --prepared "ur10e_robotiq_2f_140=$PREPARED_UR10E" \
  --stop-evidence-candidate \
    "$NATIVE_STOP_IMPLEMENTATION_BUILD" "$NATIVE_STOP_INSTALLED_TARGET" \
    "$NATIVE_STOP_RUNNER_CONTRACT" "$NATIVE_STOP_RUNNER" \
    "$NATIVE_STOP_IMPLEMENTATION_EVIDENCE" "$NATIVE_STOP_SUBSTITUTION_EVIDENCE" \
  --stop-evidence-candidate \
    "$ROS_STOP_IMPLEMENTATION_BUILD" "$ROS_STOP_INSTALLED_TARGET" \
    "$ROS_STOP_RUNNER_CONTRACT" "$ROS_STOP_RUNNER" \
    "$ROS_STOP_IMPLEMENTATION_EVIDENCE" -
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

## 2. Native topology and commands

The operator owns four processes on one runtime host: **Manager**, **Headless
API**, **Program runner**, and **Composer**. Manager owns runtime convergence
and, from the exact admitted retained bundle, owns the native execution-host
child and its private runtime channel. The operator does not fork or adopt
that child separately. Headless and Program runner expose owner facts and
intents; Composer is a stateless client. Composer binds `0.0.0.0:9000`;
Headless and Program runner bind loopback only.

Set these to the actual source-matched installation. Every path is required
by `start`; the hash values must be full lowercase SHA-256 strings. `MANAGER`
must be colocated with the source-matched `manyforge_native_execution_host`
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
PUBLICATION_ROOT=/absolute/path/to/publication
MANAGER_STATE=/absolute/path/to/manager-state
RUNTIME_STATE=/absolute/path/to/runtime-state
EXECUTION_STATE=/absolute/path/to/execution-state
SCENE_DB=/absolute/path/to/logical-scene.sqlite
HISTORIAN_DB=/absolute/path/to/historian.sqlite
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
  --package-root "$PACKAGE_ROOT" --node-catalog-path "$NODE_CATALOG" \
  --alarm-catalog-path "$ALARM_CATALOG" \
  --composer-static-dir "$COMPOSER_STATIC_DIR" \
  --canonical-empty-admission-filename "$ADMISSION_FILENAME" \
  --cell-id "$CELL_ID" \
  --publication-root "$PUBLICATION_ROOT" \
  --manager-state-root "$MANAGER_STATE" --runtime-state-root "$RUNTIME_STATE" \
  --execution-state-root "$EXECUTION_STATE" \
  --logical-scene-database "$SCENE_DB" \
  --historian-database-path "$HISTORIAN_DB" \
  --package-aggregate-sha256 "$PACKAGE_SHA256" \
  --compiler-build-manifest-sha256 "$COMPILER_SHA256" \
  --node-environment-sha256 "$NODE_ENV_SHA256" \
  --advertised-host "$ADVERTISED_HOST" \
  --expected-source-fingerprint "$SOURCE_FINGERPRINT" \
  --expected-head-commit "$MANYFORGE_COMMIT" \
  --expected-head-tree "$MANYFORGE_TREE" \
  --readiness-timeout-s 60 --poll-interval-s 0.25 \
  --reconciliation-period-ms 20 --manager-timeout-s 60
```

`--readiness-timeout-s`, `--poll-interval-s`, and
`--reconciliation-period-ms` are shown with their current defaults.
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
- Stop asks Manager for semantic shutdown, then performs bounded authenticated
  process cleanup. Only the recorded launch instance may be terminated; a
  deadline failure is `cleanup_deadline_exceeded`, not success.
- If Orin or the runtime host is unreachable, a laptop browser cannot recover
  it. Native status/stop must be run on the runtime host; split Isaac status/
  stop reports the remote-unavailable condition when it cannot prove a stopped
  record absent. Restore the host, then use the matching `status`/`stop`/`start`
  flow. Do not use broad process-name kills or SSH recipes.

## 5. Split Isaac topology and other unavailable lanes

The implemented Isaac profile is a laptop-owned outer lifecycle around the
source-matched Orin runtime. It is intentionally still a live-qualification
gate. The laptop starts `/home/tndlux/isaac-sim/isaac-sim.sh` headless with
`isaacsim.ros2.bridge` and `isaacsim.ros2.sim_control`; SSH starts the retained
ROS bridge and native ManyForge operator on Orin. The bridge barrier loads and
reads back the retained UR10 world, verifies the five
`simulation_interfaces` services and required topics, and leaves simulation
stopped. The outer owner records exact local and remote process identities,
source/artifact/world identities, readiness, failures, and cleanup results.

Use the same global state root/instance ID and provide every source-matched
remote path and digest. The command shape is:

```bash
python3 -m manyforge_operator \
  --state-root "$STATE_ROOT" --instance-id isaac --json \
  start --profile isaac --source-root "$MANYFORGE_ROOT" \
  --isaac-executable /home/tndlux/isaac-sim/isaac-sim.sh \
  --world-path /home/tndlux/workspaces/isaac_ros-dev/src/isaac_sim_custom_examples/Scenario/IsaacManipulator/UniversalRobots/isaac_manipulator_scene_light.usda \
  --local-ros-setup-script /opt/ros/jazzy/setup.bash \
  --local-ros-environment-script "$MANYFORGE_ROOT/config/ros/isaac_i9.env.sh" \
  --remote-target tndlux@192.168.1.68 \
  --remote-source-root /mnt/nova_ssd/codex-manyforge/runners/manyforge \
  --remote-manager-executable /mnt/nova_ssd/codex-manyforge/builds/current/package/bin/manyforge_manager_daemon \
  --remote-package-root /mnt/nova_ssd/codex-manyforge/builds/current/package \
  --remote-node-catalog-path /mnt/nova_ssd/codex-manyforge/builds/current/package/share/node_catalog.yaml \
  --remote-alarm-catalog-path /mnt/nova_ssd/codex-manyforge/builds/current/package/share/alarm_catalog.yaml \
  --remote-publication-root /mnt/nova_ssd/codex-manyforge/runners/state/publication \
  --remote-manager-state-root /mnt/nova_ssd/codex-manyforge/runners/state/manager \
  --remote-runtime-state-root /mnt/nova_ssd/codex-manyforge/runners/state/runtime \
  --remote-execution-state-root /mnt/nova_ssd/codex-manyforge/runners/state/execution \
  --remote-logical-scene-database /mnt/nova_ssd/codex-manyforge/runners/state/scene.sqlite3 \
  --remote-historian-database-path /mnt/nova_ssd/codex-manyforge/runners/state/history.sqlite3 \
  --remote-state-root /mnt/nova_ssd/codex-manyforge/runners/state/operator \
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

The Isaac topology is fixed to `ROS_DOMAIN_ID=0`,
`RMW_IMPLEMENTATION=rmw_fastrtps_cpp`,
`ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST`, and static peers
`192.168.1.132;192.168.1.68`. Source `config/ros/isaac_i9.env.sh` for every
participant. Isaac never auto-plays, selects a scenario, Applies a graph, or
Runs a Program. Explicit `status` and `stop` use the persisted profile and
identity; startup failure, Orin/simulator loss, explicit stop, and cleanup
failure are reported as typed stopped/degraded outcomes rather than inferred
from a port. A stopped Isaac record is re-probed against Orin before
idempotent status/stop is reported; if Orin is unreachable, the result is
`runtime_host_unavailable` rather than an unverified stopped claim. Do not
recreate this lifecycle with a historical script.

The physical ROS/real-robot lane, assistant-provider bring-up, packaged
installation, historical aggregate launchers, and old Isaac recipes are not
current operator surfaces. In particular, never use or restore historical
`scripts/launch.sh` or `scripts/lib/isaac.sh`; they are forbidden fallback
entrypoints, not alternatives to `manyforge-operator`.
