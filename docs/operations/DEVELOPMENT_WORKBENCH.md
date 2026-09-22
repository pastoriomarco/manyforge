# Development workbench

Status: current development-only entrypoint. This is not an installed product
launcher and does not authorize physical execution.

The workbench starts the current frontend with Vite hot reload and one exact
simulated native backend scenario. One command owns file placement, ports,
process order, readiness checks, scenario activation, diagnostics, and cleanup.
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

A fresh workbench cache also needs a compatible build root containing
`maintained-package/`, supplied by the maintainer/distributor with the matching
source revision. The image alone does not supply this package; if unavailable,
report the missing artifact. Do not substitute legacy examples or generate a
package with guessed inputs. `native-install/` is not imported: the launcher
builds current native owners from this checkout. It does not regenerate the
scenario package or run tests/proofs.
The setup script may recreate stale or modified dependency directories under
`.docker/`; preserve wanted local dependency edits before running it.
The selected package must contain the same Deployment revision as the matching
workspace example. If retained scenario contracts change, refresh the qualified
package cache; the launcher rejects the stale package instead of silently
combining it with current execution owners.

## Start, inspect, and stop

On the first start for a fresh cache:

```bash
MANYFORGE_DEV_QUALIFIED_BUILD_ROOT=/path/to/qualified/build \
  ./scripts/compose-dev.sh start --scenario lite6_pick_and_place
```

Later starts reuse the imported package, runtime, and npm caches:

```bash
./scripts/compose-dev.sh start --scenario lite6_pick_and_place
```

Open `http://localhost:3000`. Frontend source edits update through HMR. Backend
startup incrementally builds the current `manyforge_manager_daemon`,
`manyforge_headless_py`, `manyforge_native_execution_host`,
`manyforge_roboplan_candidate_worker` and Deployment authoring compiler targets
with their dependencies. It deliberately runs no tests, sanitizers, qualification
matrices, package materializers, or proof generators.

Inspect the exact owner-issued active tuple or stop all workbench processes:

```bash
./scripts/compose-dev.sh status
./scripts/compose-dev.sh stop
```

Each resolved immutable workbench image gets a separate cache under
`.manyforge-dev/images/`; rebuilding the mutable image tag therefore cannot
reuse native readers or npm dependencies from an incompatible runtime. Override
the cache parent, when needed, with `MANYFORGE_DEV_CACHE_BASE`.

Repeating the same `start` is idempotent. Selecting a different scenario while
the workbench is running is refused; stop it before switching. A failed new
start removes only the container it created. `stop` removes runtime state while
retaining build, npm, and imported-package caches under `.manyforge-dev/`.

The current fixed ports are frontend `3000`, Headless API `9010`, Program
runner `9011`, and internal Composer API `9020`. If one is occupied, startup
fails with a direct diagnostic rather than taking over the process.

## Operation and troubleshooting

Use Composer at `http://localhost:3000` for Program, Scene and Deployment work.
Read Runtime/history and component readiness before issuing commands. Apply
activates configuration; Save persists an artifact. Motion requires a separate
authorized command. Browser closure does not Stop the runtime, and an unconfirmed
viewer pose must not be treated as current measured state.

The ordinary authoring artifact is a Scenario project directory with fixed
members: `scenario.yaml`, `deployment.yaml`, `program.yaml`, `assignment.yaml`
and `overrides.yaml`, plus optional `sources/` and `assets/`. Open the project
directory to load those parts together. Save updates authored intent and source
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
output is inherited by the container log. Capture logs/status before `stop`,
which removes the container and runtime state; persist wanted artifacts first.
On a failed new start, the launcher prints the last logs before removing the
container it created, so retain the startup terminal output too.

| Symptom | Action |
| --- | --- |
| Image missing | Prepare dependencies and build `dev` using the commands above. |
| Package missing, incomplete or revision mismatch | Obtain matching scenario products; preserve the diagnostic and inspect the named cache. Do not bypass identity checks or delete caches blindly. |
| Port occupied | Identify the listener on 3000/9010/9011/9020; do not kill an unrelated process. |
| Different or unhealthy workbench already running | Read status/logs first. Stop only with authorization before switching scenarios; do not spawn another stack to hide the failure. |
| Slow first start | Inspect build output. Lower `MANYFORGE_DEV_BUILD_JOBS` if compilation exhausts memory; avoid repeated startup attempts. |
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

`status` must report the exact catalog, Deployment, retained bundle, graph,
Program, and Binding identities and active convergence. It reports a
`development_workbench` using a `maintained_qualified` package, while
`physical_execution_authorized` remains false because this entrypoint accepts
only a single simulated native runtime unit. Installed delivery and official
AMD64/ARM64 packages are separate release/distribution work.
