# Development workbench

Status: current development-only entrypoint. This is not an installed product
launcher and does not authorize physical execution.

The workbench starts the current frontend with Vite hot reload and one exact
simulated native backend scenario. One command owns file placement, ports,
process order, readiness checks, scenario activation, diagnostics, and cleanup.
Browser mount, reload, and disconnect are read-only reconstruction events.

## Prerequisites

Prepare the normal development image once:

```bash
./scripts/setup-docker.sh
```

A fresh workbench cache also needs an existing qualified build root containing `maintained-package/` and
`native-install/` with its exact native execution host and candidate worker.
The launcher imports these immutable products and compiles only stale current
Manager/Headless production targets in the same qualified development image.
It does not regenerate the package or rerun tests or proofs.
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
startup incrementally builds only the current `manyforge_manager_daemon` and
`manyforge_headless_py` production targets and reuses the exact qualified
native runtime; it deliberately runs no tests, sanitizers, qualification
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

## Classification and limits

`status` must report the exact catalog, Deployment, retained bundle, graph,
Program, and Binding identities and active convergence. It reports a
`development_workbench` using a `maintained_qualified` package, while
`physical_execution_authorized` remains false because this entrypoint accepts
only a single simulated native runtime unit. Installed delivery and official
AMD64/ARM64 packages are separate release/distribution work.
