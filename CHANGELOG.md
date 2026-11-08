# Changelog

All notable changes to ManyForge are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
Before 1.0, a minor release (0.x.0) may change public interfaces and file
formats; such changes are listed explicitly.

## 0.2.0 – 2026-11-02

0.2.0 rebuilds how a cell is modelled and run: every lane runs on ROS 2 with
the stock joint trajectory controller, every component follows one state
model, and planning moves to RoboPlan 0.8. Files written for 0.1.0 do not
load; start from the migrated examples in `examples/`. ManyForge is still not a
safety system ([Safety](README.md#safety)).

### Breaking changes

- **Direct Scenario references.** Older Scenario projects using assignments
  must be re-authored with direct component, target, frame, and signal references.
- **Component model v2.** Every component reports one state (lifecycle
  offline / not prepared / prepared / enabled / fault, activity idle / busy /
  unknown, stream freshness) and offers the actions Prepare, Enable, Disable
  and Recover. See [Component model](docs/reference/ARCHITECTURE.md#component-model).
- **Machines: owners and members.** A vendor machine (`ufactory.lite6_machine`,
  `ufactory.uf850_machine`) is an owner (driver process, connection,
  controller errors, power) with members (arm, controller I/O, tool I/O). A
  UFACTORY robot is always placed as a machine; members can be deactivated
  (`active: false`), not removed. See
  [Machines](docs/reference/ARCHITECTURE.md#machines-owners-and-members).
- **Derived cell latch.** The cell latches only when a component is in fault,
  offline or of unknown activity after a Stop, or faults while something
  executes; it clears by itself when every component is healthy and idle.
  Program nodes, a lost grasp and interrupt rules never fault the cell. See
  [Faults and the cell latch](docs/reference/ARCHITECTURE.md#faults-and-the-cell-latch).
- **One readiness result** gates Execute and Resume (`GET /api/program/readiness`,
  `/ws/control` key `run_readiness`): every active component enabled, no
  latch, no rule holding, required inputs fresh. See
  [Run readiness](docs/reference/ARCHITECTURE.md#run-readiness).
- **Load and Apply leave components prepared, never enabled**, on every lane.
  Press Enable, **Enable all** or **Activate and execute**; scripts, CI and
  MCP clients must call Enable after Load.
- **All lanes on ROS 2 Lyrical** with `ros2_control` and the stock joint
  trajectory controller 6.10.0: Mock (mock hardware), Isaac Sim (attach-only)
  and Real (UFACTORY xArm family). See
  [Deployment lanes](docs/reference/ARCHITECTURE.md#deployment-lanes).
- **Native lane removed:** the non-ROS replay runtime
  (`manyforge.native_execution_runtime`), its settings, Apply path, fixtures
  and the MoveIt jerk import (`--import-moveit-jerk`). Mock on ROS replaces it;
  the last commit that contains it is `0703ea48` on `dev`. "Native runtime" is
  now "execution host".
- **Close removed.** **New** empties the cell and always works; **Load** is
  New, then Apply of the chosen project. See
  [Load, New, Activate, Execute, Run](docs/operations/DEVELOPMENT_WORKBENCH.md#load-new-activate-execute-run).
- **Deployment changes** (`manyforge.deployment_intent.v2`): machine members
  with `owner`, `role` and `active`; Stop settings per robot target under
  `components[].target_settings.<target>.controlled_stop`; attachment
  `collision_exclusions`; planner, clearance and Cartesian timing defaults per
  robot. Removed: per-stream `severity` and `recovery` (only `budget_ns`
  stays), `policies.interrupt_config`, `interrupt_sources`,
  `interrupt_policies`, `idle_behaviour` and `full_stop`.
- **Program changes** (`manyforge.program.v2`, node catalog 2.9.0):
  `planning_profile` is replaced by the `planner` parameter,
  `max_blend_deviation_rad` by `blend_radius_m`; the robot-role `tcp` frame
  alias is gone (one name per frame). The 0.1.0 formats
  (`manyforge.program.v0`, `manyforge.deployment.v0`, loose files) are
  replaced by Scenario projects.
- **Removed endpoints** (Composer and Headless): all `/api/cclc/*`,
  `/api/component-overview`, `/api/execution/stop-fault` and
  `/stop-fault/recover`, `/api/execution/cell-recover` (and `/abort`),
  `/api/execution/force-close`, `/api/manager/canonical-empty-selection`.
  Composer only: `/api/activation-preparations`,
  `/api/deployment/draft/preview` and `/draft/apply` (now
  `/api/deployment/preview` and `/apply`), `/api/scenario-documents/load` and
  `/new` (now `/api/scenario/load` and `/new`), `/api/manager/apply` and
  `/api/manager/deployment-candidates/preview`. See the
  [Composer](docs/reference/REST_API.md) and
  [Headless](docs/reference/HEADLESS_REST_API.md) contracts.
- **Site settings are required for the Real lane:** robot addresses live in a
  `manyforge.site.v1` file (`MANYFORGE_DEV_SITE_SETTINGS`, read at start); a
  real machine names its key (`configuration.device_address_ref`), never the
  address. See [Real robot setup](docs/operations/REAL_ROBOT_SETUP.md#site-settings-the-robots-address).
- **xArm driver from the ManyForge fork:** the image builds
  `pastoriomarco/xarm_ros2` branch `lyrical-no-gazebo` at commit
  `46aa1147d99963a8357b89744633db428746667b` (upstream `lyrical` plus the
  `xarm_io_states` topic and the `system_control` service). The upstream
  driver lacks both. See the [workbench guide](docs/operations/DEVELOPMENT_WORKBENCH.md#install-one-build-one-start).
- **Stop modes and settings:** up to two fallback modes after the chosen one
  (`soft_on_path` → `spring_back_hold` → `spring_back_fast`), then always a
  hard stop; deceleration time default 1 s, at most 2 s; per-joint deceleration
  is written to every lane's controller (`max_deceleration_on_cancel`), so
  Isaac now uses the Deployment's values. See
  [Controlled Stop profiles](docs/operations/STOP_PROFILES.md).
- **Recovery and acknowledgement:** nothing acknowledges a Stop or a fault.
  Recover cell, Force close, Stop-fault recovery and the operator's "clear
  latch" are gone; Recover the faulted component and the latch clears.
  Component faults and the latch never clear by acknowledgement. See
  [Recovery, acknowledgement, and alarms](docs/reference/ARCHITECTURE.md#recovery-acknowledgement-and-alarms).
- **Global interrupt rules:** a trip is the operator's Stop on the rule's
  scope, then gates Resume, Execute and manual motion while the condition
  holds. "Full Stop, operator clears", the `latched` key state, the source-loss
  latch and "Motion waits" are removed; `active_at_start` defaults to `false`.
  See [Interrupts](docs/authoring/INTERRUPTS.md#4-global-rules-deployment).
- **Launcher defaults:** profile `native` is now `standard`; `--instance-id`
  defaults to `workbench` (state folder and journald tag follow it) and
  `--cell-id` to `cell`. Stop a running older workbench before the first
  0.2.0 start.
  See the [operator runbook](docs/operations/MANYFORGE_OPERATOR_RUNBOOK.md).
- **Journals reset:** run history, Stop and operation journals from earlier
  builds are not read or converted; History starts fresh.

### Added

- **History records component actions and Applies.** Every component action
  (`component_action_finished`) and every Scenario Load/New, Deployment
  Apply/Revert and Program Apply/Revert terminal (`apply_finished`) appears in
  History with its outcome and typed error code and message, including actions
  refused before they started. See
  [Runtime events](docs/operations/OBSERVABILITY.md#runtime-events).
- **Component build spec:** each component is built only from its spec; its
  fingerprint is the spec digest, so Apply restarts a component exactly when
  something it is built with changes. See
  [Component build spec](docs/reference/ARCHITECTURE.md#component-build-spec).
- **Planner block:** `search` (`rrt`, `rrt_connect`, default `rrt_connect`),
  `optimize` (`first`, `rrt_star`, `best_of_n`), collision check step and node
  budget; Deployment default with a per-motion `planner` override. See
  [Planner block](docs/reference/ROBOPLAN_ADAPTER_NOTES.md#planner-block).
- **Corner blending:** `blend_radius_m` per Follow Cartesian path move
  and per waypoint rounds corners so the tool runs through them; 0 keeps the
  exact corner. See [Cartesian paths](docs/reference/ROBOPLAN_ADAPTER_NOTES.md#cartesian-paths).
- **Cartesian timing:** `cartesian_timing` `constant_speed` (default)
  or `time_optimal`, per move with a Deployment default.
- **TCP limit inside time scaling:** every move is timed to keep the
  active TCP within 0.98 of its speed limit; the speed check runs on the
  active TCP only. See
  [Time-scaling and the TCP speed limit](docs/reference/ROBOPLAN_ADAPTER_NOTES.md#time-scaling-and-the-tcp-speed-limit).
- **Motion timing:** a per-robot Deployment setting
  `motion_timing` for joint and pose goals, shown in the Execution host
  configuration card. `smooth` is the default: TOPP-RA within the joint
  limits, then one uniform stretch under the TCP limit (gentle
  starts and stops). `fast` puts the TCP bound inside TOPP-RA,
  quicker with abrupt acceleration changes. Cartesian paths follow
  `cartesian_timing`. Changing it restarts nothing.
- **Collision clearance:** planned and validated moves keep the
  Deployment's `collision_clearance_m` (default 1 mm) from obstacles;
  per-move `clearance_m` overrides it. See
  [Collision validation](docs/reference/ARCHITECTURE.md#collision-validation).
- **Check fresh plans before dispatch:** a per-robot Deployment
  setting `collision_check_fresh_plans` (default on), shown in the Execution
  host configuration card: ManyForge's collision engine checks every waypoint
  of a newly planned move before it starts. Changing it restarts nothing. See
  [Collision validation](docs/reference/ARCHITECTURE.md#collision-validation).
- **Mount collision exclusions:** arm–tool pairs that can never touch are
  listed on the attachment (`collision_exclusions`) and not checked; the
  Lite 6 and UF850 examples carry them. See
  [Mount collision exclusions](docs/reference/ROBOPLAN_ADAPTER_NOTES.md#mount-collision-exclusions).
- **Contact pairs per request:** the active contact-grasp pairs travel with
  each plan and IK request, so a contact-policy change keeps the planner
  worker. See [Planner scene](docs/reference/ROBOPLAN_ADAPTER_NOTES.md#planner-scene).
- **Gripper Stop near the target:** a gripper already within its
  `stop_finish_tolerance` finishes its goal instead of being cancelled. See
  [Stop near the target](docs/authoring/GRIPPERS.md#stop-near-the-target).
- **Wait for the set duration:** `gripper.command_position`
  `wait_for_duration` completes after `command_duration_s` on every lane. See
  [When a gripper command completes](docs/authoring/GRIPPERS.md#when-a-gripper-command-completes).
- **Real xArm motors on only at Enable:** Prepare connects and clears
  errors with the motors off; the arm's Enable switches them on. Machine
  actions **Power off** and **Reboot**. See
  [What each action does on an xArm](docs/operations/REAL_ROBOT_SETUP.md#what-each-action-does-on-an-xarm).
- **Reset robot position on Isaac:** Isaac robots declare
  `reset_position`, like Mock: the trajectory controller off, the
  Deployment's initial joints written once through the stock writer to
  Isaac's joint commands, the controller back on to hold them. Refused with
  `simulation_not_playing` while the simulation isn't playing. See
  [Isaac lane](manyforge_isaac_adapter/lane/README.md).
- **xArm I/O from the controller's real-time report** (`xarm_io_states`,
  100 Hz): controller and tool I/O as machine members, controller-owned
  outputs read-only.
- **Components page:** component list and machine cards with state, next
  action, Prepare all / Enable all and the controller's status details.
- **Deployment Preview → Apply:** the Preview lists each component's
  disposition and whether it restarts; Apply restarts only those. Fields show
  effective values with their defaults (`GET /api/deployment/effective`).
- **Tool & I/O tab:** each signal's observed value, source and age, and manual
  output writes. Programs and manual writes share one write path
  (`POST /api/io/{signal_id}/set`).
- **Outside changes as a signal:** `<output>.changed_outside` for outputs with
  read-back, and the **Clear outside change** node. See
  [Signals](docs/authoring/INTERRUPTS.md#signals).
- **Rule control:** the **Set rule active** node, `active_at_start`, and per
  rule after the Stop `hold` or `fail_node`; Notify publishes
  `interrupt_state.<rule_id>`. See [Interrupts](docs/authoring/INTERRUPTS.md).
- **Vendor-neutral fault codes** (`estop`, `protective_stop`,
  `limit_violation`, `drive_fault`, `communication`, `external_control`, …)
  with the vendor's original code alongside.
- **Point-cloud source component** (`manyforge.pointcloud_source`) on every
  lane, shown in the 3D view.
- **Ordinary-motion jerk limits** on every lane when the Deployment switch is
  on (planned by RoboPlan; nothing extra is sent to ROS).
- **Tools as components:** grippers mounted on a robot flange, per-component
  TCPs and default TCP, the **Active TCP** and the **Set TCP** node; payload
  follows attach and detach. See [TCPs](docs/authoring/TCPS.md).
- **Pneumatic grippers on I/O:** valve outputs and optional open, closed,
  pressure and workpiece-grasped inputs from any I/O component; each gripper
  node picks its `required_inputs`. See [Grippers](docs/authoring/GRIPPERS.md#pneumatic-grippers).
- **Grasp lost** as a Program event (`on_grasp_lost`): Stop, detach at the
  observed pose, typed outcome. See
  [Grasp lost](docs/authoring/INTERRUPTS.md#grasp-lost-as-a-program-event).
- **Component streams:** per-stream freshness budgets; a robot's or I/O
  device's stale stream is a fault, a sensor's only shows stale.
- **Isaac pose source** as a `pose_source` Deployment component, with
  per-object pose settings on the Object tab.
- **Measured ghost:** the live viewer shows the measured robot pose with its
  freshness.
- **Robots as data:** component descriptors and packages under `components/`.
  See [Adding a component](docs/authoring/ADDING_A_COMPONENT.md).
- **Composer Scenario projects** (`scenario.yaml`, `deployment.yaml`,
  `program.yaml`, `assignment.yaml`) with Draft, Apply, Discard, Save, Save
  As, Load and Export; project folders outside the repository from the site
  file. See [Project folders](docs/operations/DEVELOPMENT_WORKBENCH.md#project-folders).
- **Programs:** persistent Blackboard keys, expressions, node rules (Move
  until signal, Probe search) and automatic motion retries. See
  [Blackboard keys](docs/authoring/BLACKBOARD_KEYS.md) and
  [Expressions](docs/authoring/EXPRESSIONS.md).
- **AI model components:** `ai_model` Deployment components (several per
  Deployment) for the Composer AI tab; tokens only from `MANYFORGE_AI_*`
  variables. See the [assistant cookbook](docs/operations/ASSISTANT_E2E_COOKBOOK.md).
- **Runtime history** with a disk bound and up to five previous sessions;
  logs, evidence and motion traces are disk-bounded too. See
  [Observability](docs/operations/OBSERVABILITY.md).
- **Development workbench:** `./scripts/compose-dev.sh build` and
  `start` / `status` / `stop` on one ROS 2 Lyrical image for every lane. See
  the [workbench guide](docs/operations/DEVELOPMENT_WORKBENCH.md).
- **Example Scenarios** for Mock (`mock_*`), Isaac (`sim_*`) and Real
  (`real_*`), including Lite 6 pneumatic pick and place on the real robot.

### Changed

- **RoboPlan 0.8.0:** constant TCP speed on linear moves; free-space paths
  differ for the same seed; IK restarts randomize only the group's joints.
- **Rounded Cartesian corners are resolved finely:** with `constant_speed`,
  a move with a blend radius is resolved at least every twentieth of the arc
  radius, so it no longer slows down as a whole.
- **Pre-dispatch collision check:** runs on the trajectory's own waypoints
  (the clearance covers the spline) for cached moves, moves with a contact
  policy and, unless the robot's Deployment turns it off, fresh plans; a
  failing fresh plan is replanned.
- **Isaac Sim is attach-only:** you start Isaac Sim and open the scene;
  ManyForge never loads a world or plays, pauses or resets the simulation by
  itself. Replaces 0.1.0's simulator reset with
  `simulation_control.reset.resume_play_after_reset`. See
  [Stack setup](docs/operations/STACK_SETUP.md).
- **A simulation that isn't playing** never blocks Enable, Activate or
  Execute; new motion on it is refused as an operator Stop
  (`simulation_not_playing`), so Program nodes can play the simulation again.
- **Lanes replace 0.1.0's deployment modes** (`composer_only`,
  `commanded_sim`, `real_robot`): each component names its `lane` (`mock`,
  `isaac`, `real`).
- **Real robot lifecycle:** 0.1.0's supervised Lite 6 transport and owner
  liveness rules are replaced by the machine's Prepare and Recover, which
  restart the driver when it is not connected.
- **Transport controls:** **Execute** starts the Program held and each
  motion group has **Run**, **Stop**, **Resume**, **Halt** and **Reset**
  (0.1.0 had one Run/Stop/Reset pill).
- **Manual output writes** are refused only while motion executes or the cell
  is latched; the last write wins.
- **A failed Apply step** no longer stops the others: every affected
  component is still prepared and the result lists each outcome; nothing is
  rolled back.
- **New and Load ask once** when the cell isn't empty, listing what they stop,
  plus the unsaved-Draft warning.
- **Example Scenarios renamed** by lane prefix and scope; artifact ids are
  unchanged.

### Removed

- 0.1.0's native execution backend for simulated lanes: replaced by the Mock
  lane on ROS.
- Pre-dispatch collision sampling **between** a trajectory's waypoints (along
  the executed spline, at the collision-check step): removed with the
  collision clearance; the check now covers the trajectory's own waypoints
  (10 ms grid) and the collision clearance covers the spline's deviation.
  Last implemented on `dev` at commit `0703ea48` (introduced in `53cc751e`; not in 0.1.0):
  `manyforge_core/src/validation_sampling.cpp`,
  `manyforge_core/include/manyforge_core/trajectory/validation_sampling.hpp`,
  and its use in `component_lifecycle_plan_validity.cpp`.
- 0.1.0's runtime status popup (`/api/runtime/status`) and
  `POST /api/runtime/external/{provider_id}`: replaced by `GET /api/components`,
  the `/ws/control` key `component_state` and the Components page.
- 0.1.0's unified launcher (`scripts/launch.sh`, `scripts/demo-*.sh`,
  `scripts/lib/`): replaced by `./scripts/compose-dev.sh` and
  `manyforge-operator`.
- 0.1.0's assistant supervisor probe (`assistant_supervisor.py`) and the
  legacy mux provider: replaced by `ai_model` components and their endpoint
  stream health.
- 0.1.0's topic-based command-gate node: command admission is the Kernel's.
- 0.1.0's `ExecutionKernel::acknowledge_intervention` and the Run-side
  auto-acknowledge: replaced by the derived cell latch.
- 0.1.0's real-robot cell file (`manyforge.real_robot_cell.v1`) and the Lite 6
  supervised commissioning example: replaced by machines and the `real_*`
  Scenarios.
- The loose compiled example Deployments (`examples/*.deployment.yaml`):
  replaced by the Scenario examples.
- The "Universal Robots I/O states" and "Universal Robots set_io" signal
  sources (no model used them; there is no real UR lane).
- `Dockerfile.ros-lyrical-real-xarm` and `docker-compose.realtime.yaml`: one
  Lyrical image serves every lane.
- The leftover Force close machinery (the Kernel's `arm_force_close` and
  `force_close_operations`, the execution host's `force_close` command) and
  its error codes.

### Fixed

- The runtime lookahead no longer stops a move that leaves a contact:
  it compares the plan with itself.
- Component actions are no longer refused when unrelated ROS nodes start or
  stop: only the publisher count of the component's own topics counts.
- An interrupt rule sees every write to a key it reads, in order, before the
  next write or motion: short pulses still trip it.
- A gripper Stop at the end of its stroke no longer races the stock
  controller's cancel.
- A gripper Stop no longer hangs when its cancel arrives just as the goal
  finishes (the stock controller then sends no result): the controller's
  acceptance of the cancel ends the command, and the Isaac gripper bridge now
  holds the fingers before accepting, like the stock controller. A Stop
  also sends no cancel when the controller reported the goal reached or the
  node's `command_duration_s` has passed.
- A rule with `hold` now holds the run when it trips while nothing in its
  scope moves, instead of failing the next motion node.
- xArm: an output write answered `ret=1` counts as accepted; an output shown
  unknown clears when the I/O stream reports it.
- An e-stop held during recovery, or a degraded Stop, no longer wedges the
  lifecycle or silently closes motion admission.
- An Apply that fails before any effect releases the authoring claim.
- An Apply that fails after it started changing the cell no longer locks
  authoring until a restart (`authoring_not_ready`): New (and Load, which
  starts with a New) releases its claim and empties the cell.
- Kernel exit after about 20 Load cycles (Apply journal checked at startup).
- A Stop journaled under a stale owner epoch no longer locks the cell
  ("waiting for durable record").
- A leaked gripper action node and its exit crash.
- Pneumatic finger collision state, a Program cycle Reset race, UR10e Isaac
  Load (`relative_references`), Program editor redraw on every status update.

### Known issues

- Fast DDS 3.6.2 can crash a ROS process with SIGSEGV while it shuts down
  ([eProsima/Fast-DDS #6599](https://github.com/eProsima/Fast-DDS/issues/6599));
  see [Troubleshooting](docs/operations/TROUBLESHOOTING.md#component-process-crashes-on-shutdown).
- rclcpp 32.0.x (Lyrical) can abort a controller process with
  "ExecutorNotifyWaitable::is_ready: Internal error, got index out of range" while it
  shuts down (about 1 in 50 teardowns; no effect on the Load or Stop). Fixed upstream in
  rclcpp 33.1.0 ([ros2/rclcpp #3174](https://github.com/ros2/rclcpp/pull/3174)).
- `time_optimal` Cartesian moves can overshoot the planned TCP speed between
  trajectory samples by up to about 1.4×; `constant_speed` (the default) does
  not. See [RoboPlan adapter notes](docs/reference/ROBOPLAN_ADAPTER_NOTES.md).
- **Many different Loads:** the deployment content stores refuse new content
  after about 1,024 Loads with different content in one Workbench session
  (repeated Loads of the same content don't count). Stop and start the
  Workbench to clear it. Fix planned for 0.2.1.
- **Fast DDS shared memory:** a killed ROS process can leave a stale Fast DDS
  shared-memory port, after which long-running ManyForge processes stop reaching
  whichever component reuses that participant id. The workbench therefore runs
  DDS over UDP (`FASTDDS_BUILTIN_TRANSPORTS=UDPv4`). Under
  `ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST`, rmw_fastrtps forces shared memory
  back on, so prefer the default (`SUBNET`).
- **Rare controller freeze:** in Load soak tests, the stock `ros2_control_node`
  froze about 3 times in 55 teardowns (joint states stop, SIGINT ignored); the
  Load recovered through the controller process kill. Under investigation.
- **Isaac speed:** the arm controllers follow wall-clock time; keep Isaac Sim at
  about real time or moves can abort on path tolerance. See
  [Troubleshooting](docs/operations/TROUBLESHOOTING.md#isaac-slower-than-real-time).

## 0.1.0 – 2026-07-29

First tagged release (`v0.1.0`, `main` at `8ae05265`): the publication-readiness
baseline and the hardening done before the tag. Items that 0.2.0 removes or
replaces are listed in its Removed and Changed sections.

### Added

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
  non-starvation.
- **Kernel API**: `ExecutionKernel::acknowledge_intervention(principal_id, reason)`
  is the explicit, audited path for clearing `kInterventionRequired`. It emits
  a `kInterventionAcknowledged` event with the supplied principal and reason,
  mirroring the existing `acknowledge_completed` pattern.
- **Cycle-manager Run-side auto-ack**: when the kernel reports
  `InterventionRequired` on Run from `STOPPED` and the leftover state is
  provably execution-local (no active streaming/direct-control lease, no
  running intervention session), the cycle manager automatically
  acknowledges it through the new kernel API. Otherwise
  Run fails closed with a structured reason.
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
  is spec-driven by the maintainer's private specifications, and development
  without them is limited to bounded fixes/proposals until maintainer audit.
- **RoboPlan scene-lock contention narrowed**: the speed-validation loop in
  `RoboPlanPlanningContext::validate_trajectory` now acquires the scene mutex
  once across all waypoints instead of per-iteration. Full FK/IK parallelism
  with planning remains blocked on RoboPlan 0.3.0 exposing a thread-local
  Pinocchio `Data` view.
- `RecoveryTrigger` enum aligned with the canonical taxonomy: `EXECUTE_TIMEOUT` and `POSTCONDITION_FAILED` added. The legacy
  `TIMEOUT`/`NODE_FAILURE` aliases were subsequently removed entirely (see
  Removed); persisted traces carry trigger strings outward only and are never
  parsed back into the enum.
- Cycle-manager state-machine documentation now shows
  the real five-state model (`IDLE/RUNNING/STOPPED/INTERRUPT_HANDLER/`
  `DIRECT_CONTROL`) instead of the legacy three-state diagram.
- `manyforge_ros_bringup`: license declaration changed from `Proprietary` to
  `Apache-2.0`; maintainer email updated.
- Composer monitor popup (`RuntimeStatusOverlay`) is now visible on every
  launcher invocation; rows adapt to the profile detected from the deployment
  YAML.

### Removed

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
  sessions) and not a recovery trigger. **Migration**:
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
  audit boundary. Callers must now call
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
