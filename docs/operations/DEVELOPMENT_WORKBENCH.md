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

Existing v1 authored Deployment files must be migrated explicitly: change their
intent version to v2 and remove the authored `cell_id`. Do not remove cell IDs
from compiled Deployment artifacts or rename existing runtime journals.

## Start, inspect, and stop

Start an empty authoring workbench:

```bash
./scripts/compose-dev.sh start
```

Or select one repository project folder:

```bash
./scripts/compose-dev.sh start --project examples/lite6_pick_and_place.scenario
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

Plain `stop` discards runtime state. To stop the workbench and preserve its
diagnostic state, use:

```bash
./scripts/compose-dev.sh stop --keep-evidence
```

The command copies nothing until Docker confirms that the container stopped.
It then writes `.manyforge-dev/evidence/<UTC timestamp>.partial/`, containing the
`manyforge-state/` tree except what is reproducible from the image and the
Scenario: `installed-targets/`, `artifacts/raw-artifacts/` (model and mesh
copies), `artifacts/prepared-activations/` (activation inputs embedding
them) and `execution-run/manyforge-retained-planning-*` (unpacked planning
robot packages); the Manager journal keeps their digests,
plus `docker.log` and a bounded `docker-inspect.json` summary containing the
image, mounts and exit state (not container environment variables). Once
complete, it renames the bundle to `<UTC timestamp>/`, removes the container
and prints that path. Evidence is not deleted automatically. If collection or
finalization fails, the command exits non-zero, keeps the stopped container and
may leave the partial directory.
If only container removal fails, the completed bundle and stopped container are
both retained. When set, `MANYFORGE_DEV_CACHE_BASE` places the bundle under its
own `evidence/` directory instead.

Each resolved immutable workbench image gets a separate cache under
`.manyforge-dev/images/`; rebuilding the mutable image tag therefore cannot
reuse native readers or npm dependencies from an incompatible runtime. Override
the cache parent, when needed, with `MANYFORGE_DEV_CACHE_BASE`.

Repeating the same `start` is idempotent. Selecting a different project while
the workbench is running is refused; stop it before switching. A failed new
start removes only the container it created. `stop` removes runtime state while
retaining build, npm, and repository-bootstrap caches under `.manyforge-dev/`.

Composer settings (viewer display toggles, tree editor link style and start
direction, Motion panel layout and confirmations, and profiles; the gear at
the top right) are installation-specific and survive stop/start. They are kept in
`.manyforge-dev/state/composer/settings.json`, mounted into the workbench as
`/var/lib/manyforge/composer` (`MANYFORGE_STATE_DIR=/var/lib/manyforge`). Only
that folder persists; all other runtime state still resets with the container.
An unreadable settings file is never overwritten silently: Composer uses the
defaults, keeps settings read-only and offers a reset that keeps the file as a
`settings.json.bak-<time>` backup.

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
output is inherited by the container log. Capture logs/status before plain
`stop`, which removes the container and runtime state; persist wanted artifacts
first, or use `stop --keep-evidence` for the diagnostic bundle described above.
On a failed new start, the launcher prints the last logs before removing the
container it created, so retain the startup terminal output too.

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
`physical_execution_authorized` remains false because this entrypoint accepts
only a single simulated native runtime unit. Installed delivery and official
AMD64/ARM64 packages are separate release/distribution work.
