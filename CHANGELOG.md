# Changelog

All notable changes to ManyForge will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

No ManyForge version has been tagged yet. Until `v0.1.0` is cut, this file is
a pre-release release-note draft: every entry below belongs to the upcoming
`0.1.0` release, and none should be read as shipped version history.

Pre-1.0 releases (0.x.y) signal that the public API may change between minor
versions; we still try to keep changes contained and document any breaking
ones explicitly.

## 0.1.0 - Upcoming

First publishable release. This section accumulates all work intended for the
initial `v0.1.0` tag, including the publication-readiness baseline and later
hardening done before the first tag was granted.

### Fixed

- Scene admission identity survives bounded history cleanup, so later attachment
  requests retain the same owner. Unsettled claim releases keep their durable
  proof until exact reconciliation succeeds.

### Added

- Program node groups can be selected with multiple checkboxes, inherited from
  a branch, or left in All only, with a shortcut to group creation. Motion-panel
  settings prevent hiding the last visible control covering any move and show
  group motion counts and target/component roles. Older layouts reveal All
  when necessary without changing saved preferences or runtime admission.

- Reset/Halt accept owner-confirmed fully held mixed selections (Ready plus
  Stopped work). The control parser no longer disconnects on that valid state;
  partially held, stopping, faulted and unavailable selections remain unsettled.

- Missing Program pose goals use a complete local pose seed, so editing one
  coordinate or adding a frame can create a valid goal. Merely opening the
  editor still leaves the Draft unchanged.

- Manual motion may use the Applied configuration while authored edits remain
  unapplied, with explicit runtime-versus-Draft warnings. Manual-operation
  progress now updates through bounded existing control-stream deltas. Capturing
  a move's current pose resolves the Applied robot binding and preserves the
  observed base frame instead of confusing Program and manual-control roles.
  Move-to shortcuts use Applied Program goals and runtime-published object
  positions, never Draft positions. Live progress comes from the native events
  response's in-memory samples, not the lifecycle-only journal.

- **Contained-Apply workbench evidence preservation**: after an unknown-effect
  or later-phase Apply failure retains runtime restrictions, the simulated
  development workbench can stop with `--keep-evidence`, retain its quiescent
  runtime state and container diagnostics, and then cold-start the Saved
  Scenario project without carrying locks or execution into the replacement.
  A known pre-effect refusal remains an ordinary correctable refusal.
  When a New/Load/Apply reply is lost, Composer finds its Apply by the exact
  browser request id, so the failure notice still appears. New with Empty
  Deployment from a populated cell is a documented known limitation.
- **Structured backend fault taxonomy (arm + gripper)**: ros2_control backend
  faults carry a stable `reason_code` (e.g. `tracking_error_exceeded`,
  `gripper_goal_rejected`) with measurements through the execution digest
  (`errorDetails`), with operational severities and externally publishable
  `MANYFORGE_*` fault codes. Gripper faults that previously surfaced as bare
  `tree_failed` prose are now classified end-to-end. A C++/Python parity test
  guards the taxonomy against drift.
- **Standard ROS 2 diagnostics surface**: `manyforge_ros.diagnostics_bridge`
  publishes `diagnostic_msgs/DiagnosticArray` on `/diagnostics` from the
  Composer digest (one status per active fault, taxonomy-mapped, OK heartbeat
  when healthy). Depends only on `diagnostic_msgs` — works with any
  diagnostics consumer.
- **ros2_medkit integration (day 0)**: new `manyforge_medkit_adapter` package
  forwards core faults to an AUTOSAR-DEM-style fault manager (FAILED on
  latch, PASSED healing stream on clear, CRITICAL bypasses debounce, SOVD
  discovery hint), verified against a real `fault_manager` lifecycle.
  Install/test/operate runbook at `docs/operations/ROS2_MEDKIT.md`.
- **Failure archives + event-recorder hardening**: ERROR-class incidents
  persist a freeze-frame archive across reloads; the event recorder writes
  off-lock via an async JSONL writer, rate-limits observation polls, and
  synthesizes semantic execution events across poll-skip windows so terminal
  transitions are never silently dropped.
- **Scheduler priorities**: `IScheduler::schedule(...)` accepts a new
  `SchedulerPriority` argument (default `Safety`, backwards-compatible for
  existing callers). `SimpleScheduler` runs `Safety` and `Monitoring`
  callbacks on independent worker threads so a Safety callback blocked on a
  long-running lock cannot starve a Monitoring watchdog. The planning-timeout
  watchdog in `ExecutionKernel::execute()` is now tagged `Monitoring`. New
  regression test
  `NativeSchedulerTest.MonitoringLaneFiresWhileSafetyLaneIsBlocked` enforces
  non-starvation. See `manyforge_specs` ADR-0019 and `spec/220` §5.5.
- **Kernel API**: `ExecutionKernel::acknowledge_intervention(principal_id, reason)`
  is the explicit, audited path for clearing `kInterventionRequired`. It emits
  a `kInterventionAcknowledged` event with the supplied principal and reason,
  mirroring the existing `acknowledge_completed` pattern.
- **Cycle-manager Run-side auto-ack**: when the kernel reports
  `InterventionRequired` on Run from `STOPPED` and the leftover state is
  provably execution-local (no active streaming/direct-control lease, no
  running intervention session), the cycle manager automatically
  acknowledges it through the new kernel API. Otherwise
  Run fails closed with a structured reason. See `spec/400` §6.6.2 and the
  audit-K4 decision.
- **Direct-control and streaming observability**: direct-control WebSocket
  paths now have bounded timing checks, structured event draining, rejection
  reasons, and stress coverage for end/new-begin races.
- **Runtime velocity scaling**: in-flight trajectories are retimed when the
  operator changes the velocity scale, instead of stopping and restarting the
  action. Streaming backends also read the runtime scale per dispatch tick.
- **Native Composer viewer and direct jog surfaces**: Composer gained native
  robot/object control affordances, cartesian field jogs through the
  follow-target stream, command-authority gating, and bounded async-store
  behavior tests.
- **Move-To ghost visualization**: unreachable Move-To previews now render
  the full gripper visual subtree where available, rather than relying only on
  the TCP link mesh.
- **Cylinder scene primitive path**: scene authoring and backend surfaces now
  keep cylinder primitives available where RoboPlan supports them.
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
  - Runtime status surface (`/api/runtime/status`) with the profile-aware
    popup pill + per-component rows.
  - External-supervisor registration endpoint
    (`POST /api/runtime/external/{provider_id}`) so launchers can feed
    additional component status into the popup.
  - File-backed failure archives with age + byte-budget FIFO cleanup.
  - Heartbeat endpoint (`/api/infra/heartbeat`) for external watchdogs.
  - AssistantOverlay gated on `deployment.assistant.enabled`.
- **Assistant modes + bridge** (`manyforge_assistant_bridge`)
  - Bounded-autonomy bridge serving the assistant-provider HTTP contract
    against an OpenAI-compatible upstream (vLLM).
  - Two production lanes: `nemoclaw` (direct vLLM, port 8100) and `openclaw`
    (sandboxed OpenClaw gateway, port 8200 - production default since
    2026-05-07).
  - Mode-scoped MCP wrapper enforcing per-mode tool catalog.
  - Periodic `/compact` neutralization of chain-session cascades (iter-32
    production recipe).
  - New `assistant_supervisor.py` long-running probe poster that feeds
    Composer's runtime-status surface with vLLM / proxy / OpenClaw / bridge
    readiness rows.
- **Behavior + Composer viewer** (`manyforge_behavior`, `manyforge_composer`)
  - py_trees-based behavior-tree runner with `CycleManager`
    (run/pause/stop/reset, intervention sessions).
  - Native 3D scene visualization and control-panel surfaces.
- **ROS bringup** (`manyforge_ros`, `manyforge_ros_bringup`)
  - Order-tolerant `isaac_demo_supervisor` ROS node that loads the Composer
    program/deployment once Composer + Isaac + arm/gripper actions are all
    reachable, then triggers a home reset through the normal execution
    backend.
  - Isaac gripper action bridge translating `control_msgs/GripperCommand` into
    Isaac's raw `JointState` command topic.
  - Topic-based command-gate node (fail-closed by default).
- **Unified launcher** (`scripts/launch.sh`)
  - NemoClaw-style 7-step wizard with review gate.
  - Lane (manyforge-only | isaac-sim) x assistant (on | off) selection via
    flags or interactive prompts.
  - Nine named scenarios including Isaac+AI
    (`isaac-ur10e-light-scene-assistant`) and the
    `ur5-simple-pick-place` 3-step test fixture. Old short names
    (`pick-place`, `ur10e`, `shape-pick`, `vision-pose`,
    `assistant-authoring`, `isaac-light`, `isaac-assistant`) still resolve via
    an alias table with a one-time deprecation warning per launch.
  - Concern-organised lib modules under `scripts/lib/`.
  - Seven `scripts/demo-*.sh` collapsed to thin shims.
  - Idempotent re-runs against partial state via lane-aware
    `expected_port_owner`.

### Changed

- Program property editors settle after validation changes instead of repeatedly
  rendering on callback changes. Unfinished sibling edits stay visible and block
  Apply across row reorder, map-key rename and equal owner readback; explicit
  Discard still clears them. Missing-value defaults and effect replay no longer
  reset staged input.

- **Real-robot candidate and observed-state authority**: approved Robot Control
  candidates now carry the same TCP and per-joint dynamics caps through
  preview, approval recheck, and execution. ROS-backed physical execution
  requires a fresh measured endpoint before accepting controller success, and
  terminal/idle scene projection continues following measured robot state with
  truthful source age while preserving execution metadata. The catalog is
  `0.5.0`; in-tree programs were migrated to that exact version.
- **Real-Lite6 lifecycle progress remains observable during bounded vendor
  operations**: lifecycle primitives and command-gate renewals now run on
  separate serialized NRT workers, provider status/authority/mutation
  callbacks use independent groups, and Composer status reads no longer wait
  behind authority-session transport I/O. Late or post-deadline primitive
  completions are audited without mutating a replaced transaction.
- **Real-Lite6 owner liveness is independent from robot telemetry
  freshness**: a transient stale robot report still fences motion immediately
  but no longer misclassifies an advancing, identity-valid ROS owner as dead
  or revokes its authority session. Owner-publication delivery uses the
  generated cell heartbeat budget. Robot-observation delivery separately uses
  the generated critical-progress budget, capped by the producer's 1000 ms
  observation lease, so ordinary 400 ms xArm report gaps do not become false
  re-arm events. Generic admission publication retains its independent 250 ms
  lease, while the internal xArm write gate now follows the producer's 1000 ms
  observation lease; neither can outlive fresh robot state. Stalled owner
  publication, genuinely stale robot observations, and expired
  lifecycle-command deadlines remain fail-closed.
- **Real-Lite6 supervised transport loss is terminal and fail-closed**:
  generated deployments retain the 250 ms command-admission lease and add a
  2000 ms report/control-loss timeout. Prolonged stale reports or failed
  control reads require a fresh controller-manager process; reconnecting a
  cable cannot silently reuse or re-arm the stale SDK session.
- **Composer editor chrome restructured (industrial-HMI pass)**: the
  full-width bottom bar is replaced by a slim top header (program name,
  passive applied/saved status dots, Save/Save As/Load as uniform icon
  buttons with instant tooltips, Discard/Revert/Apply with Apply as the
  single accent), a floating view-options pill on the graph canvas, and a
  pendant-style transport pill (Run/Stop/Reset + velocity) docked
  bottom-center of the 3D viewer — width-stable across state changes, color
  reserved for state (Run green only when armable, Stop the only red
  control, Reset amber when it would discard live cycle state). Node
  palette drag previews are node-sized, link handles are larger, node
  category colors are richer, and viewer side panels scroll instead of
  covering the transport controls.
- **Launcher forwards frontend dev-build env vars**
  (`MANYFORGE_FRONTEND_SOURCE_MODE`, `MANYFORGE_FRONTEND_FORCE_REBUILD`)
  into both lane containers — previously a worktree dev build requested at
  the launcher silently fell back to committed HEAD inside the container.
- **Native viewer is the sole render path**: `hmiAvailable`/`hmi_available`
  renamed to `viewerAvailable` across Composer models, routes, and frontend;
  the `hmi_snapshot` node image source is now `viewer_snapshot` (node catalog
  `0.2.0`). The VLM snapshot broker fails closed when no browser client is
  connected.
- **Simulator reset resumes play by default**: for commanded-sim deployments
  the reset flow re-asserts `playing` after readiness and reopens command
  authority only after play is restored (`simulation_control.reset.
  resume_play_after_reset`, default `true`; explicit opt-out for deployments
  that deliberately operate paused). Fixes the paused-simulator lockout after
  a looping program's trailing reset.
- **`before_planning` pose-source latch widened**: every enabled detached
  object bound with the policy is refreshed at each planning latch (not only
  objects referenced by the move being planned), because planning
  collision-checks the whole scene; missing or stale observations fail the
  latch closed instead of planning against a phantom pose.
- **Legacy mux/plugin provider quarantined**: `configure-manyforge-provider.sh`
  and related mux paths refuse to run unless
  `MANYFORGE_ENABLE_LEGACY_MUX_PROVIDER=true`; the assistant-provider bridge
  lanes are the only default path.
- **Hot-path performance hardening**: the ros2_control execution monitor uses
  condition-variable waits instead of polling sleeps (sub-10 ms stop/cancel
  reaction), pybind execution surfaces release the GIL, and hot-path
  diagnostics are structured and rate-limited.
- Clarified contributor and LLM-agent guidance: ManyForge implementation work
  is spec-driven by the private `manyforge_specs` repo, and development
  without that repo is limited to bounded fixes/proposals until maintainer
  audit.
- **RoboPlan scene-lock contention narrowed**: the speed-validation loop in
  `RoboPlanPlanningContext::validate_trajectory` now acquires the scene mutex
  once across all waypoints instead of per-iteration. Full FK/IK parallelism
  with planning remains blocked on RoboPlan 0.3.0 exposing a thread-local
  Pinocchio `Data` view - see `manyforge_specs` ADR-0019.
- `RecoveryTrigger` enum aligned with the `spec/460` §6.2.2 canonical
  taxonomy: `EXECUTE_TIMEOUT` and `POSTCONDITION_FAILED` added. The legacy
  `TIMEOUT`/`NODE_FAILURE` aliases were subsequently removed entirely (see
  Removed); persisted traces carry trigger strings outward only and are never
  parsed back into the enum.
- Cycle-manager state-machine documentation in `spec/400` §6.6.2 now shows
  the real five-state model (`IDLE/RUNNING/STOPPED/INTERRUPT_HANDLER/`
  `DIRECT_CONTROL`) instead of the legacy three-state diagram.
- `manyforge_ros_bringup`: license declaration changed from `Proprietary` to
  `Apache-2.0`; maintainer email updated.
- Composer monitor popup (`RuntimeStatusOverlay`) is now visible on every
  launcher invocation; rows adapt to the profile detected from the deployment
  YAML.

### Removed

- **Native simulator execution core (major change).** Commit `1c5b9e39` is the
  last commit that contains the native in-process (RoboPlan) execution
  simulator and its native host lane. From the next commit on, every lane —
  offline mock, Isaac Sim and real robots — executes through ROS 2
  (`ros2_control` with the stock JointTrajectoryController; mock hardware for
  the offline lane), as decided for the R4 shared-execution design. The
  transition began earlier (custom controller plugin, in-loop motion step and
  Isaac command gate already removed); this step deletes the simulator
  backend, its lifecycle ports, the native host executables and their
  native-only tests and configuration records. Lane-neutral components that
  only carry "native" in their name (for example the cell runtime owner) and
  every wire/durable identifier are kept.
- **Legacy Viser/HMI stack**: `manyforge_hmi/`, `ViserPanel`, the `viser`
  dependency, launcher/compose wiring, and the HMI test lane are deleted; the
  native Composer viewer owns the operator and assistant-visible render
  surface.
- **`SkillDefinition.expands_to` compatibility alias** removed from live
  REST/MCP assistant surfaces (canonical `body`/`expandsTo` contract only).
- **`RecoveryTrigger.TIMEOUT` / `RecoveryTrigger.NODE_FAILURE`** deprecated
  aliases removed; the parser rejects the legacy strings.
- **MCP intervention surface**: `manyforge_intervention` execute action no
  longer accepts `trigger_reason="manual_request"`. The MCP allowlist now
  mirrors `RecoveryTrigger.value` exactly; `manual_request` is a
  `session_kind` concept (carried automatically by `manual_intervention`
  sessions) and not a recovery trigger per `spec/460` §6.2.2. **Migration**:
  external MCP clients that explicitly passed `trigger_reason="manual_request"`
  should omit the field (the default for `manual_intervention` is unchanged)
  or pass a canonical recovery-trigger value when starting a
  `delegated_recovery` session.

### Contract changes before first tag

- **Cycle Stop now halts preparation-phase executions**: `cycle.stop()`
  previously paused the kernel only when it was already `EXECUTING`; a stop
  landing during `PLANNING`/`VALIDATING` left the async worker free to
  dispatch the motion after the cycle had reported stopped (observed live:
  the arm moved for ~11 s post-Stop). Preparation-phase executions are now
  stopped outright (Run-from-STOPPED replans from fresh state, so nothing
  is lost) and the preparation→executing race is closed with a bounded
  re-check.
- `TrajectoryCache` revision-less `store`/`lookup` overloads are removed:
  every stored trajectory must declare the collision-world revision it was
  validated against; planning/execution hits use `lookup_with_revision`,
  existence probes use `contains`, and the explicitly named
  `peek_unvalidated` serves observability/debug reads only.
- Collision-validation rejections no longer pay a duplicate legacy
  collision-compare query: the `legacy_collision_compare_*` diagnostic keys
  are gated behind `ExecutionKernelConfig::
  enable_legacy_collision_compare_diagnostics` (default off).
- The execution digest promotes classified `reason_code` tokens (including
  the new gripper classes) to the top-level `errorCode`, and external
  diagnostics surfaces now latch faults from any non-`running` digest state —
  tree-terminal failures resting in `idle` are no longer invisible to
  `/diagnostics` and the medkit adapter.
- `ExecutionKernel::execute()` now rejects with `kErrorInvalidRequest`
  (details: `reason_code=intervention_required`) while the kernel is in
  `kInterventionRequired`. The previous implicit clearing of intervention by
  re-issuing `execute()` is removed to preserve the operator-intervention
  audit boundary (audit K4). Callers must now call
  `acknowledge_intervention(principal_id, reason)` before re-running. Cycle
  manager and Composer Run route already do this transparently when the
  leftover state is execution-local.

### Tests and qualification

- Added C++ lifecycle, streaming, scheduler, ROS soft-stop, and soak bounds
  regression coverage.
- Added Composer frontend tests for command authority, async-store stale
  response handling, direct-control observability, and bounded polling surfaces.
- Added backend tests for URDF/gripper visualization routes and Composer
  dependency coverage.

### Repository hygiene

- Added LICENSE (Apache 2.0) + NOTICE (third-party attribution).
- Added top-level VERSION file as the version source of truth.
- Synchronised version `0.1.0` across all declaration sites
  (`CMakeLists.txt`, `pyproject.toml`, `package.xml`, `setup.py`,
  `package.json`, frontend lockfile).
- Added CHANGELOG.md, ROADMAP.md, CONTRIBUTING.md, SECURITY.md.
- Added GitHub PR + issue templates.
- Added CI workflows: Python tests, frontend build, bash syntax, private-spec
  boundary, version sync (in addition to the existing cpp-tsan lane).
- Composer package metadata now declares the URDF/mesh parsing dependencies
  used by the native viewer and gripper-limit validation path.
- AGENTS.md and README updated with the new `dev`-branch workflow and a
  "For LLMs" entry point pointer.
