# Changelog

All notable changes to ManyForge will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Pre-1.0 releases (0.x.y) signal that the public API may change between
minor versions; we still try to keep changes contained and document any
breaking ones explicitly.

## [Unreleased]

Work in progress on the `dev` branch — see ROADMAP.md for direction.

### Changed

- Clarified contributor and LLM-agent guidance: ManyForge implementation
  work is spec-driven by the private `manyforge_specs` repo, and
  development without that repo is limited to bounded fixes/proposals
  until maintainer audit.

## [0.1.0] — 2026-05-14

First publishable release. Retrospective entry covering the major milestones
that landed before the unified launcher + publication-readiness pass.

### Added

- **Core kernel** (`manyforge_core`)
  - Bounded-stop / pause / reset state machine with `InterventionRequired`
    terminal state for software-stop failures.
  - Execution timeout enforcement with bounded drain.
  - Collision-monitoring policy with configurable period.
  - Late-callback guards so a stale FJT result cannot overwrite a
    monitor-triggered failure.
  - Resync-after-intervention protocol gated by a fresh observed state.
  - Thread-safe `ManualScheduler` (callbacks fire outside the scheduler lock);
    explicit `ExecutionKernel` worker join before member teardown.

- **RoboPlan adapter** (`manyforge_roboplan`)
  - Native execution backend for simulated lanes.
  - `ros2_control` execution backend with `commanded_sim` and `observed_robot`
    source modes, tracking-error monitor, peak-tracking-error telemetry, and
    `resync_required` recovery.
  - Direct use of upstream RoboPlan `Scene::addMeshGeometry` API (the LIFO
    workaround for missing-upstream-API is retired).
  - Support-contact collision policy with per-query allowance (no global
    scene-mutating sweeps in the hot path).

- **Composer** (`manyforge_composer`)
  - FastAPI backend + React/ReactFlow frontend for behavior-tree authoring.
  - Three deployment modes: `composer_only`, `commanded_sim`, `real_robot`
    (detected from deployment YAML).
  - Runtime status surface (`/api/runtime/status`) with the
    profile-aware popup pill + per-component rows.
  - External-supervisor registration endpoint
    (`POST /api/runtime/external/{provider_id}`) so launchers can feed
    additional component status into the popup.
  - File-backed failure archives with age + byte-budget FIFO cleanup.
  - Heartbeat endpoint (`/api/infra/heartbeat`) for external watchdogs.
  - AssistantOverlay gated on `deployment.assistant.enabled`.

- **Assistant modes + bridge** (`manyforge_assistant_bridge`)
  - Bounded-autonomy bridge serving the assistant-provider HTTP contract
    against an OpenAI-compatible upstream (vLLM).
  - Two production lanes: `nemoclaw` (direct vLLM, port 8100) and
    `openclaw` (sandboxed OpenClaw gateway, port 8200 — production default
    since 2026-05-07).
  - Mode-scoped MCP wrapper enforcing per-mode tool catalog.
  - Periodic `/compact` neutralization of chain-session cascades
    (iter-32 production recipe).
  - New `assistant_supervisor.py` long-running probe poster that feeds
    Composer's runtime-status surface with vLLM / proxy / OpenClaw /
    bridge readiness rows.

- **Behavior + HMI** (`manyforge_behavior`, `manyforge_hmi`)
  - py_trees-based behavior-tree runner with `CycleManager`
    (run/pause/stop/reset, interrupt handling, intervention sessions).
  - Viser-based 3D HMI with live scene + robot visualization, control panel.

- **ROS bringup** (`manyforge_ros`, `manyforge_ros_bringup`)
  - Order-tolerant `isaac_demo_supervisor` ROS node that loads the Composer
    program/deployment once Composer + Isaac + arm/gripper actions are all
    reachable, then triggers a home reset through the normal execution
    backend.
  - Isaac gripper action bridge translating `control_msgs/GripperCommand`
    into Isaac's raw `JointState` command topic.
  - Topic-based command-gate node (fail-closed by default).

- **Unified launcher** (`scripts/launch.sh`)
  - NemoClaw-style 7-step wizard with review gate.
  - Lane (manyforge-only | isaac-sim) × assistant (on | off) selection
    via flags or interactive prompts.
  - Nine named scenarios including Isaac+AI (`isaac-ur10e-light-scene-assistant`)
    and the `ur5-simple-pick-place` 3-step test fixture. Old short
    names (`pick-place`, `interrupt`, `ur10e`, `shape-pick`, `vision-pose`,
    `assistant-authoring`, `isaac-light`, `isaac-assistant`) still resolve
    via an alias table with a one-time deprecation warning per launch.
  - Concern-organised lib modules under `scripts/lib/`.
  - Seven `scripts/demo-*.sh` collapsed to thin shims.
  - Idempotent re-runs against partial state via lane-aware
    `expected_port_owner`.

### Changed

- `manyforge_ros_bringup`: license declaration changed from `Proprietary`
  to `Apache-2.0`; maintainer email updated.
- Composer monitor popup (`RuntimeStatusOverlay`) is now visible on every
  launcher invocation; rows adapt to the profile detected from the
  deployment YAML.

### Repository hygiene (publication prep)

- Added LICENSE (Apache 2.0) + NOTICE (third-party attribution).
- Added top-level VERSION file as the version source of truth.
- Synchronised version `0.1.0` across all 10 declaration sites
  (`CMakeLists.txt`, `pyproject.toml`, `package.xml`, `setup.py`,
  `package.json`).
- Added CHANGELOG.md, ROADMAP.md, CONTRIBUTING.md, SECURITY.md.
- Added GitHub PR + issue templates.
- Added CI workflows: Python tests, frontend build, bash syntax, private-spec
  boundary, version sync (in addition to the existing cpp-tsan lane).
- AGENTS.md and README updated with the new `dev`-branch workflow and a
  "For LLMs" entry point pointer.

[Unreleased]: https://github.com/pastoriomarco/manyforge/compare/v0.1.0...dev
[0.1.0]: https://github.com/pastoriomarco/manyforge/releases/tag/v0.1.0
