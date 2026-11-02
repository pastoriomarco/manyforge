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

The current checkout runs as a source-built **development workbench** with
three lanes, all ros2_control with the stock joint trajectory controller:
Mock, Isaac (a separately running Isaac Sim; the workbench never starts it)
and Real (UFACTORY xArm-family robots; Lite 6 measured on hardware). There is
no packaged installer, and physical execution is not qualified. Do not infer
physical authorization from a green UI.

Read [the workbench runbook](docs/operations/DEVELOPMENT_WORKBENCH.md) first.
It is the public source for prerequisites, startup, ports, caches and diagnosis.
Despite its name, operating this workbench requires no private development docs.

Use a Linux host with Docker Engine, Compose v2, BuildKit, Git and Python 3,
network access for dependencies, and sufficient disk/RAM for native compilation.
The workbench builds its runtime capabilities and canonical-empty bootstrap
from this checkout. A selected Scenario is an ordinary project folder; startup
without a selected project opens an empty authoring workspace.

From a new checkout, after authorization to install/start:

```bash
git clone --branch dev https://github.com/pastoriomarco/manyforge.git
cd manyforge
./scripts/compose-dev.sh build
./scripts/compose-dev.sh start \
  --project examples/mock_lite6_pick_and_place.scenario
```

`build` prepares pinned dependencies and builds the full ROS Lyrical image
(`Dockerfile.ros-lyrical-full`, with the pinned controller overlay and the xArm
driver); it may recreate stale/modified dependency trees under `.docker/`.
The `dev` Compose service is the ROS-free test toolchain, not the workbench.
Do not run it over dependency edits you need to preserve. Startup incrementally
builds required native targets and starts one workbench; an optional project is
loaded through the ordinary application path;
it does not run tests or qualify a release. Subsequent starts reuse the cache:

```bash
./scripts/compose-dev.sh start
./scripts/compose-dev.sh start --project examples/mock_lite6_pick_and_place.scenario
./scripts/compose-dev.sh status
```

Open **http://localhost:3000**. Check reported scenario, active configuration and
convergence, not just whether the page loads. Do not start a second stack to
work around an unhealthy one.

On a Jetson, `build` selects NVIDIA's `noble-jetpack` package suite by itself
(`NVIDIA_ISAAC_ROS_RELEASE_SUITE` overrides it).

### Configure

- **Site settings** (machine facts, never in a Scenario): a JSON file
  `{"schema":"manyforge.site.v1","device_addresses":{"lite6_main":"192.168.1.31"}}`,
  optionally with `project_folders` (absolute host paths). Export its absolute
  path as `MANYFORGE_DEV_SITE_SETTINGS` before `start`; it is read only at
  start. A Real machine's card names the `device_addresses` key
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
  `GET /api/cell/latch`, `GET /api/program/readiness` on Composer (9020).
  A C2 error after an e-stop is normal: Recover the machine once the e-stop is
  released (with authorization). On Isaac, "Simulation not playing" never
  blocks Execute; new motion is refused until the simulation plays.
- Runtime/history exposes failures; Robot Control is for addressed manual commands.
  A stale/disconnected viewer is not evidence of current robot pose. Stop issuing
  new motion requests and inspect owner status when observation is uncertain.
- For API work, use the generated contracts linked below or the running schemas:
  Headless `http://localhost:9010/openapi.json`, Composer
  `http://localhost:9020/openapi.json`. Do not guess endpoints.
- First diagnosis is read-only:

```bash
./scripts/compose-dev.sh status
docker logs --tail 200 manyforge-development-workbench
docker inspect --format '{{.State.Status}} {{.State.ExitCode}}' manyforge-development-workbench
```

The default container name can be overridden with `MANYFORGE_DEV_CONTAINER_NAME`;
use that same name for diagnosis. Capture logs before stopping: stop removes the
container while keeping its durable runtime root. Save wanted artifacts through the UI first.
After explicit shutdown authorization:

```bash
./scripts/compose-dev.sh stop
```

Closing the browser does not stop execution. Container shutdown is not a substitute
for an operational Stop or a hardware emergency stop. Builds, npm dependencies and
repository bootstrap caches survive shutdown. See the runbook for startup failures and
scenario switching; do not automatically restart/reapply to hide a fault.

### Authority and safety

- Runtime use does not require the private development repository.
- Use the current generated [REST contract](docs/reference/REST_API.md) and
  [optional MCP contract](docs/reference/MCP_RUNTIME_CONTRACT.md), and the
  [architecture map](docs/reference/ARCHITECTURE.md).
- Observe owner-issued status. A lost response is not proof that a command
  failed; do not repeat effects or infer physical rest from a receipt.
- Do not start motion, Apply, Load, New, Reset, Enable, Recover, clear faults or
  overwrite artifacts without authorization. Never bypass admission, Stop or recovery prerequisites.
- [scripts/templates/AGENTS.md](scripts/templates/AGENTS.md) is a shipped
  runtime-agent template, not repository development guidance.
  Material under `agent-skills/` is not a current tool contract.

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
