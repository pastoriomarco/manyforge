# ManyForge operator runbook

> **Current boundary (O1.3):** the `native` operator entrypoint is
> implemented at the source boundary below. Live product qualification is
> still open. `isaac` is unavailable until O1.4 lands; this document must not
> be read as an Isaac availability claim.

## 1. Preconditions and identity

Run the operator on the selected ManyForge runtime host. Orin is the default
native runtime/UI host; the source checkout, installed dependencies, manager
executable, catalogs, and all state paths below must be on that host. Use
absolute paths. The source must be clean: the launcher collects
`manyforge.source_identity.v1` and rejects a dirty checkout.

The current native launcher source anchor is:

| Input | Value |
|---|---|
| ManyForge commit | `13f9131888c8f740bc172b9e89d00b308f973cec` |
| ManyForge tree | `8556e53f72e53cc8d440e1e80cb18ebf36dd9722` |
| Source fingerprint | `041149aa4f472e4c00ffa92db94a88249f8b7b9b416204812d391258a503f803` |

The three artifact identity inputs are mandatory and must come from the
source-matched build/package record; do not invent or reuse values:
`package_aggregate_sha256`, `compiler_build_manifest_sha256`, and
`node_environment_sha256`. O1.3 native records a `mounted_install`, not a
container image, so no container image ID is supplied to this native command.
An O1.4 Isaac image/world identity must be published before that profile is
usable.

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
  --expected-source-fingerprint "041149aa4f472e4c00ffa92db94a88249f8b7b9b416204812d391258a503f803" \
  --expected-head-commit "13f9131888c8f740bc172b9e89d00b308f973cec" \
  --expected-head-tree "8556e53f72e53cc8d440e1e80cb18ebf36dd9722" \
  --readiness-timeout-s 60 --poll-interval-s 0.25 \
  --reconciliation-period-ms 20
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
- Partial startup or component loss becomes `degraded` or a recorded stopped
  startup failure. Stop and restart with the same complete identity set; the
  launcher has no automatic component restart.
- Stop asks Manager for semantic shutdown, then performs bounded authenticated
  process cleanup. Only the recorded launch instance may be terminated; a
  deadline failure is `cleanup_deadline_exceeded`, not success.
- If Orin or the runtime host is unreachable, a laptop browser cannot recover
  it and the current O1.3 launcher makes no remote-host recovery claim. Restore
  the host, run `status` locally, then use the matching `stop`/`start` flow.
  Do not use broad process-name kills or SSH recipes.

## 5. Isaac and other unavailable lanes

The split Isaac topology is **deferred and unavailable at this base**. The
intended O1.4 shape is: the source-matched ManyForge composition and Composer
run on Orin; a laptop-side outer lifecycle owner starts the installed
`${HOME}/isaac-sim` process, verifies the Jazzy bridge,
`simulation_interfaces`, and the retained UR10 world, and keeps simulation
stopped until an explicit simulator action. The future profile must also
record its exact Fast DDS/RMW/static-peer, image, and world identities; those
inputs are not supplied by O1.3. This topology is not an O1.3 command or
qualification claim. At this base, selecting `--profile isaac`
returns the stable `isaac_profile_unavailable` result (exit `78`) before
starting Isaac or any ManyForge process.

When O1.4 is integrated, the required Isaac lifecycle is still fail-closed:
startup failure, explicit overall stop, and leaving an active simulator
session stop Isaac; after a deliberate Program Run it returns to stopped.
Isaac must never auto-play or remain active outside an active session. Isaac
loss must be reported as unavailable/degraded rather than inferred from a
port. Until that implementation lands, do not attempt to reconstruct this
lifecycle with another script.

The physical ROS/real-robot lane, assistant-provider bring-up, packaged
installation, historical aggregate launchers, and old Isaac recipes are not
current operator surfaces. In particular, never use or restore historical
`scripts/launch.sh` or `scripts/lib/isaac.sh`; they are forbidden fallback
entrypoints, not alternatives to `manyforge-operator`.
