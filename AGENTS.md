# ManyForge — agent landing

**Installing, starting or operating ManyForge? Use the operator quickstart below.
No private repository is required.**

**Here to change code, tests or development tooling? Start with
[manyforge_specs/AGENTS.md](../manyforge_specs/AGENTS.md), not the runtime
templates.** That file owns development rules and routes you to the canonical
implementation procedure and current plan. See
[Code changes](#code-changes--mandatory-development-entrypoint) below if the
sibling checkout is missing. Do this before development edits, builds or tests;
the documented operator setup/build commands below do not require that detour.

ManyForge is the deployment-side code and runtime repository. Start with
[README.md](README.md) and [the operator documentation index](docs/INDEX.md)
for installation, supported entrypoints and operation.

## Runtime and operator work

### Install and start the available runtime

ManyForge runs as one **workbench** container, with three lanes, all
ros2_control with the stock joint trajectory controller: Mock, Isaac (a
separately running Isaac Sim; the workbench never starts it) and Real
(UFACTORY xArm-family robots; Lite 6 measured on hardware). Physical execution
is not qualified. Do not infer physical authorization from a green UI.

Read [the workbench runbook](docs/operations/DEVELOPMENT_WORKBENCH.md) first.
It is the public source for prerequisites, install options, startup, ports,
caches and diagnosis. Despite its name, operating this workbench requires no
private development docs.

**Prerequisites.** A Linux host, amd64 or arm64 (Jetson AGX Thor and Orin
with JetPack 7.2.1 included), with Docker Engine and Python 3, network access
and at least 10 GB of free disk. Options 2 and 3 also need Git and 16 GB of
RAM for the C++ build; option 3 also needs the Compose v2 plugin and BuildKit.

**Install options.** Use option 1 unless a row below says otherwise.

| Option | Use it when | Commands |
| --- | --- | --- |
| 1. Run the prebuilt ManyForge image (default) | Using ManyForge. No checkout; nothing compiles. The host reaches `ghcr.io`. | Copy the launcher kit out of `ghcr.io/pastoriomarco/manyforge:<VERSION>` (below), then `./manyforge run` |
| 2. Pull the ROS image, build from the checkout | Developing ManyForge from a release checkout (`main` or a `vX.Y.Z` tag). | `./scripts/compose-dev.sh pull`, then `./scripts/compose-dev.sh start` |
| 3. Build the ROS image from source | `Dockerfile.ros-lyrical-full` or what it installs was changed, the checkout is not a release, or `pull` fails. Takes about an hour. | `./scripts/compose-dev.sh build`, then `./scripts/compose-dev.sh start` |
| 4. Operator launcher (advanced) | Only when the user asks for a digest-checked package served on the LAN at port 9000, or a remote Docker host next to Isaac Sim. | [Operator runbook](docs/operations/MANYFORGE_OPERATOR_RUNBOOK.md) |

Option 1, after authorization to install/start, in an empty folder:

```bash
mkdir -p ~/manyforge && cd ~/manyforge
docker run --rm --entrypoint tar ghcr.io/pastoriomarco/manyforge:<VERSION> \
  -c -C /workspace VERSION manyforge scripts/compose-dev.sh scripts/lib manyforge_operator/run_session.py \
  | tar -x
./manyforge run --project examples/mock_lite6_pick_and_place.scenario
./manyforge status
```

`ghcr.io/pastoriomarco/manyforge:<VERSION>` is published with each release
(amd64 and arm64; no login). It holds ManyForge's programs and built GUI on
the reference ROS image. `./manyforge run` starts the image of the folder's
`VERSION` (pulling it when absent; `run <reference>` or `run sha256:<digest>`
names one exact image), with Composer serving the GUI and API at
**http://localhost:9000**. `--project` loads a Scenario at start: a bundled
`examples/<name>.scenario` (read-only in the image) or the absolute host path
of a project in one of the `project_folders` of the site settings, where
Scenarios are saved; without it the workspace starts empty.
`./manyforge status`, `./manyforge logs` and `./manyforge stop` inspect, show
the log of and stop it. To update: `stop`, copy the launcher kit out of the new
version's image, `run`.

Options 2 and 3, from a new checkout:

```bash
git clone https://github.com/pastoriomarco/manyforge.git
cd manyforge
./scripts/compose-dev.sh pull
./scripts/compose-dev.sh start \
  --project examples/mock_lite6_pick_and_place.scenario
./scripts/compose-dev.sh status
```

For option 3, run `./scripts/compose-dev.sh build` in place of the `pull`
line. What the commands do:

- `pull` fetches `ghcr.io/pastoriomarco/manyforge-ros:<VERSION>` (the version
  in the checkout's `VERSION` file; amd64 and arm64; no login) and tags it
  `manyforge-ros-lyrical-full`. `pull <reference>` or `pull sha256:<digest>`
  fetches one exact image instead.
- `build` prepares pinned dependencies and builds the same image from
  `Dockerfile.ros-lyrical-full` (ROS 2 Lyrical from source, the xArm driver);
  it may recreate stale/modified dependency trees under `.docker/`, so do not
  run it over dependency edits you need to preserve. On a Jetson it needs no
  extra argument.
- `start` first builds ManyForge's own programs from the checkout in a
  separate step (the ROS image holds none; the first start takes much longer
  than later ones, and the start timeout bounds readiness only), then starts
  one workbench. An optional project is loaded through the ordinary
  application path. It does not run tests or qualify a release. A selected
  Scenario is an ordinary project folder under `examples/`; without
  `--project` it opens an empty authoring workspace.
- The `dev` Compose service is the ROS-free test toolchain, not the workbench.

**Verify.** `run`, `start` and `status` print the current state as JSON:
check `"ready": true` (for `run --project`, also the reported `scenario`; for
`start`, `"unavailable_reasons": []` and the reported Scenario and
convergence), not just whether the page loads. Then open
**http://localhost:9000** (option 1) or **http://localhost:3000** (options 2
and 3). Do not start a second stack to work around an unhealthy one.
Subsequent starts reuse the cache:

```bash
./scripts/compose-dev.sh start
./scripts/compose-dev.sh start --project examples/mock_lite6_pick_and_place.scenario
./scripts/compose-dev.sh status
```

**Update to a new version** (options 2 and 3), after authorization:
`./scripts/compose-dev.sh stop`, `git pull`, `./scripts/compose-dev.sh pull`
(option 3: `build`, needed only when the Dockerfile or what it installs
changed), then `start`. A new image compiles ManyForge's programs again at its
first start.

**Fall back.** If `run` cannot pull its image, use option 2 or 3 from a
checkout. If `pull` fails (no network, or no image published for the
checkout's `VERSION`), use option 3: both write the same image tag, and the
one run last is what `start` uses. If `run` or `start` fails, read its output
and the runbook's troubleshooting table before changing anything.

### Configure

- **Site settings** (machine facts, never in a Scenario): a JSON file
  `{"schema":"manyforge.site.v2","device_addresses":{"lite6_main":"192.168.1.31"}}`,
  optionally with `project_folders` (absolute host paths) and the disk limits
  (`disk_max_percent`, `disk_always_free_bytes`, `disk_machine_minimum_bytes`;
  built-in defaults apply when absent). Export its absolute
  path as `MANYFORGE_SITE_SETTINGS` before `./manyforge run` (option 1) or as
  `MANYFORGE_DEV_SITE_SETTINGS` before `./scripts/compose-dev.sh start`; it is
  read only at start. A Real machine's card names the `device_addresses` key
  (`configuration.device_address_ref`), never the address.
- **ROS/DDS**: `ROS_DOMAIN_ID` (default 0) must match Isaac Sim and other ROS
  machines; leave `ROS_AUTOMATIC_DISCOVERY_RANGE` unset or `SUBNET` to reach
  them on the LAN (`LOCALHOST` isolates a test machine unless
  `ROS_STATIC_PEERS` lists the hosts). A running container keeps the values it
  was started with.
- **Deployment**: each component has a `lane` (`mock`, `isaac`, `real`); a
  UFACTORY robot is a machine whose owner carries the lane and address, with
  members (`arm`, `controller_io`, `tool_io`) that can be deactivated
  (`active: false`) but not removed.
- The full variable list is in the
  [workbench guide](docs/operations/DEVELOPMENT_WORKBENCH.md#environment-variables).

### Operate, diagnose and shut down

- Use Composer's Program, Scene, Components and Deployment workspaces. Apply
  changes runtime configuration; Save persists artifacts. **New** empties the
  cell; **Load** is New then Apply, and leaves components prepared. Prepare /
  Enable (or **Activate and execute**) bring components up; **Execute** starts
  the Program held, and **Run** starts each motion group. On a real xArm the
  motors switch on only at the arm's Enable. Inspect status and any
  confirmation before proceeding. Motion requires separate explicit
  authorization and readiness.
- Component states, the cell latch and Execute readiness: `GET /api/components`,
  `GET /api/cell/latch`, `GET /api/program/readiness` on Composer (9020; 9000 with option 1).
  A C2 error after an e-stop is normal: Recover the machine once the e-stop is
  released (with authorization). On Isaac, "Simulation not playing" never
  blocks Execute; new motion is refused until the simulation plays.
- Runtime/history exposes failures; Robot Control is for addressed manual commands.
  A stale/disconnected viewer is not evidence of current robot pose. Stop issuing
  new motion requests and inspect owner status when observation is uncertain.
- For API work, use the generated contracts linked below or the running schemas:
  Headless `http://localhost:9010/openapi.json`, Composer
  `http://localhost:9020/openapi.json` (option 1: port 9000). Do not guess
  endpoints.
- First diagnosis is read-only:

```bash
./scripts/compose-dev.sh status
docker logs --tail 200 manyforge-development-workbench
docker inspect --format '{{.State.Status}} {{.State.ExitCode}}' manyforge-development-workbench
```

With option 1, `./manyforge status` and `./manyforge logs --tail 200` do the
first two; its container is `manyforge` and its data folder `.manyforge/` in
the launcher folder (options 2 and 3: `.manyforge-dev/`). The default
container name can be overridden with `MANYFORGE_DEV_CONTAINER_NAME`
(`MANYFORGE_CONTAINER_NAME` with option 1); use that same name for diagnosis.
Option 1 and the development workbench never run at the same time: each
launcher refuses while the other kind is running. Capture logs before stopping: stop removes the
container while keeping its durable runtime root. Save wanted artifacts through the UI first.
After explicit shutdown authorization:

```bash
./scripts/compose-dev.sh stop     # option 1: ./manyforge stop
```

Closing the browser does not stop execution. Container shutdown is not a substitute
for an operational Stop or a hardware emergency stop. Builds, npm dependencies and
repository bootstrap caches survive shutdown. See the runbook for startup failures and
scenario switching; do not automatically restart/reapply to hide a fault.

### Authority and safety

- Runtime use does not require the private development repository.
- To drive ManyForge over the API, follow
  [Use ManyForge over the API](docs/INDEX.md#use-manyforge-over-the-api-scripts-and-ai-agents):
  the [change-request rules](docs/reference/REST_API.md#change-requests), a
  worked example script, then the generated [REST contract](docs/reference/REST_API.md),
  [optional MCP contract](docs/reference/MCP_RUNTIME_CONTRACT.md) and
  [architecture map](docs/reference/ARCHITECTURE.md).
- Observe owner-issued status. A lost response is not proof that a command
  failed; do not repeat effects or infer physical rest from a receipt.
- Do not start motion, Apply, Load, New, Reset, Enable, Recover, clear faults or
  overwrite artifacts without authorization. Never bypass admission, Stop or recovery prerequisites.
- [scripts/templates/AGENTS.md](scripts/templates/AGENTS.md) is a shipped
  runtime-agent template, not repository development guidance.

## Code changes — mandatory development entrypoint

Read [manyforge_specs/AGENTS.md](../manyforge_specs/AGENTS.md), especially its
[foundational engineering rules](../manyforge_specs/AGENTS.md#foundational-engineering-rules-mandatory),
before implementation or review. Follow its instruction chain to the
[implementation workflow](../manyforge_specs/docs/implementation/manyforge-implementation-workflow.md).
Detailed engineering policy, commands and temporary plan exceptions belong
there, not in deployment/operator documentation.

Exact development status, supported qualification lanes and residuals are owned
by the [plan index](../manyforge_specs/docs/plans/PLANS_INDEX.md), its owning
plan and linked evidence. Do not maintain a second status narrative here.

Prefer the sibling checkout `../manyforge_specs`, or
`${HOME}/workspaces/dev_ws/src/manyforge_specs`. The repository is
`https://github.com/pastoriomarco/manyforge_specs`. If unavailable, runtime
operation and read-only inspection remain self-contained. Keep small fixes
bounded and report the limitation; spec-governed behavioral or architectural
changes require maintainer audit before merge. Do not guess missing contracts.

## Repository boundaries and authorization

- Keep private development references out of runtime code, operator docs and
  shipped templates. Run `scripts/ci/check-private-spec-boundary.sh` when
  touching that boundary. This landing file is an explicit routing exception.
- Generated public contracts come from their code owners; do not hand-copy specs.
- Preserve existing user changes. Do not delete or overwrite unrelated work.
- Commit, push, PR creation and release operations each require explicit human
  authorization. A request to implement a change does not authorize them.
- Public branch/contribution rules are in [CONTRIBUTING.md](CONTRIBUTING.md).
