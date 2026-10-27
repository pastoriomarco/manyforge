# ManyForge

ManyForge is a robot-programming and execution framework. Composer provides a
visual editor for Programs, Scenes and Deployments; the backend plans and
executes motions, monitors the cell and reports operation status. A Program
can be used with different compatible Deployments without embedding a robot
backend in the Program.

The source workbench uses ROS 2 controller adapters. It supports a mock lane
and can connect to a separately running Isaac Sim scene. Physical robot use
requires the appropriate robot integration and attended hardware qualification;
a simulated run does not qualify a physical robot.

## Start the workbench

Follow the [workbench guide](docs/operations/DEVELOPMENT_WORKBENCH.md) for
host prerequisites and the matching ROS image. Once the image is available,
start from the repository root:

```bash
./scripts/compose-dev.sh start --project examples/lite6_pick_and_place.scenario
```

Open **http://localhost:3000**. Omit `--project` to start with an empty
workspace. Use `./scripts/compose-dev.sh status` to inspect the workbench and
`./scripts/compose-dev.sh stop` to stop it. Starting the workbench does not run
its test suite or authorize robot motion.

For Isaac, open the matching scene and its ROS simulation interfaces in Isaac
Sim separately, then load the corresponding ManyForge Scenario. The workbench
connects to the scene already open in Isaac; it does not start Isaac or load the
world. Configure ROS discovery so the two hosts can communicate. Check component
and stream status before executing.

## Using ManyForge

A Scenario project contains the Program, Deployment, assignment and referenced
assets. **Save** persists authored changes; **Apply** makes the selected
configuration available to the runtime. Loading or applying a Scenario does not
execute its Program. Inspect component readiness and Runtime/history before
issuing motion, and do not infer command success from a lost connection.

Configured host project folders appear in Composer as `/projects/<name>`. Use
the displayed container path or a relative path in the picker, not a pasted host
path. After changing configured folders, stop the workbench and start it again
to update their mounts; running `start` on an already running workbench does
not change them. See [Project folders](docs/operations/DEVELOPMENT_WORKBENCH.md#project-folders).

Previous session journals are kept under the workbench's runtime state root.
Retention is limited; see the [session note](docs/operations/FRESH_CHECKOUT_CHECKLIST.md)
if you need to copy records for longer-term use.

The optional Composer AI tab uses your OpenAI-compatible model server,
configured through an `ai_model` component and an assistant profile. Its
tools edit the Draft; review those edits and Apply or Discard them yourself.
See the [assistant setup guide](docs/operations/ASSISTANT_E2E_COOKBOOK.md).

See the [documentation index](docs/INDEX.md) for the workbench, authoring and
API references. The [interrupts guide](docs/authoring/INTERRUPTS.md) explains
signals, node rules and global rules.

## Architecture

The Program runner orchestrates Program nodes. The ROS-free execution kernel
owns motion execution and Stop semantics; ManyForge's collision layer validates
and monitors motions. Deployment and component owners manage the selected
configuration and component lifecycle. Composer presents and edits those owners'
state; it is not the authority for robot motion.

## Safety

ManyForge does **not** implement functional safety. Its software Stops,
command gates and acknowledgement controls are not safety-rated functions.
Integrate appropriate safety measures, including hardware emergency stops and
safety-rated controls, and perform a risk assessment before using real
hardware. Closing the browser does not stop the runtime.

## Contributing and project information

See [CONTRIBUTING.md](CONTRIBUTING.md) for development and test instructions.
Agents should also read [AGENTS.md](AGENTS.md). The current version is in
[VERSION](VERSION); release notes are in [CHANGELOG.md](CHANGELOG.md).

ManyForge is licensed under Apache 2.0; see [LICENSE](LICENSE) and
[NOTICE](NOTICE). Report security issues as described in [SECURITY.md](SECURITY.md).
