# Changelog

All notable changes to ManyForge are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
Before 1.0, a minor release (0.x.0) may change public interfaces and file
formats; such changes are listed explicitly.

## [0.1.0] - 2026-10-30

First public release.

### Added

- **Composer**, a browser-based editor for Programs, Scenes and Deployments
  with a 3D viewer of the cell. Edits go to a Draft; **Apply** makes the
  configuration available to the runtime, **Discard** drops the Draft, and
  **Save**, **Save As**, **Load** and **Export** work on Scenario projects
  (`scenario.yaml`, `deployment.yaml`, `program.yaml`, `assignment.yaml`,
  plus optional sources and assets). Export copies a portable project with
  its referenced files. **New** opens an empty, read-only "Untitled"
  Scenario that can be composed and applied at once; Save As names and
  places it.
- **Programs as behavior trees**: motion, gripper, I/O, logic and control-flow
  nodes; Blackboard keys, including `persistent` keys that keep their values
  across restarts; and expressions for conditions and values.
- **Interrupts**: signals, node rules (Move until signal, Probe search) and
  Deployment-wide global rules. See [Interrupts](docs/authoring/INTERRUPTS.md).
- **Run, Stop and Reset** with a speed override, and operator Stop policies
  that decide which Program work is held.
- **Controlled Stop profiles** per Deployment: `soft_on_path`,
  `spring_back_hold` and `spring_back_fast`, optional natural completion on a
  short remaining path, and escalation to a hard-stop stage. See
  [Controlled Stop profiles](docs/operations/STOP_PROFILES.md).
- **Manual motion** from the viewer and Motion panel: jogging, Move-to
  shortcuts and capturing the current pose into a move. Manual motion uses
  the Applied configuration and warns when the Draft differs.
- **Motion planning and collision checking** with RoboPlan: planned motions
  are validated against the Scene before execution and monitored while they
  run.
- **Grippers**: position grippers, and pneumatic grippers mapped to Boolean
  I/O outputs with optional `open`, `closed`, `pressure` and
  `workpiece_grasped` inputs that each gripper node can require. See
  [Grippers](docs/authoring/GRIPPERS.md).
- **Three execution lanes in one ROS 2 Lyrical image**, all running through
  `ros2_control` with the stock joint trajectory controller:
  - **Mock**: simulated controller hardware, no robot or simulator needed.
  - **Isaac Sim (attach-only)**: you start Isaac Sim and open the scene;
    ManyForge connects to it and reads object poses from the scene.
  - **Real**: UFACTORY Lite 6 through the unmodified UFACTORY xArm ROS 2
    driver included in the image.
- **Component status and lifecycle** for each Deployment component, with
  readiness, stream warnings and faults shown in Composer.
- **Runtime history**: a searchable event history with bounded disk use, and
  retention of up to five previous sessions.
- **Project folders outside the repository**: list host folders in the
  per-machine site file; they appear in Composer as `/projects/<name>`.
- **Optional AI assistant**: the Composer AI tab connects to your own
  OpenAI-compatible model server, configured as an `ai_model` component. The
  assistant edits the Draft only through ManyForge's tools; you review and
  Apply or Discard its changes. See the
  [assistant cookbook](docs/operations/ASSISTANT_E2E_COOKBOOK.md).
- **APIs**: the Composer and Headless REST APIs, with OpenAPI schemas, and an
  optional MCP interface. See the [documentation index](docs/INDEX.md).
- **Development workbench**: `./scripts/compose-dev.sh build` builds the
  image once; `start`, `status` and `stop` run the workbench. Logs, evidence
  captures and motion-trace snapshots have disk bounds. See the
  [workbench guide](docs/operations/DEVELOPMENT_WORKBENCH.md).
- **Example Scenarios** in `examples/` for the Mock, Isaac Sim and Real lanes,
  including pneumatic-gripper pick and place.

### Changed

- **Operation journal v17** (`execution-state/operation-journal.sqlite3`):
  each Stop record is stored once and each participant Stop plan once per
  Stop, so a Stop record is several times smaller. Journals written by
  earlier development builds are refused, not converted. Each workbench
  start begins with a new journal and archives the previous one, so this
  affects only an archived journal that is opened by hand.

### Known limitations

- ManyForge is **not a safety system**. Its Stops and command gates are not
  safety-rated. A robot installation needs its own safety-rated controls,
  emergency stops and risk assessment.
- ManyForge runs as a source-built development workbench on Linux with host
  networking. There is no packaged installer and no update or rollback
  procedure yet. Run it only on a trusted network.
- **Real robots**: the UFACTORY Lite 6 is the only supported real model.
  The driver also supports xArm 5/6/7 and UF850, but ManyForge does not
  support them yet.
- **Robots and tools**: you choose robots from the included arm and gripper
  profiles. You cannot yet add a robot from a MoveIt configuration, mount
  separate tools or change tools at runtime.
- **Isaac Sim**: ManyForge does not start Isaac Sim or load worlds. Signal-based
  pneumatic gripper I/O is available on Mock and Real only.
- **Grippers**: the Deployment editor cannot add a gripper to an arm-only
  Deployment; start from an example that has one. A lost grasp is not
  available as a per-node Program event.
- **Component management**: components are managed through the Deployment
  editor and component status rows; there is no separate Component Manager.
- Trusted external (third-party) Program nodes are not available.
- Records from previous sessions that were not imported into the history are
  not guaranteed to survive retention. Copy any session you need to keep.
