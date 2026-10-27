# Troubleshooting — Symptom → Cause → Diagnostic Step

> **Cutover status:** retained as a restoration and behavior reference. The
> command recipes below are not qualified on the current `dev` branch and
> must not be executed until the owning H0/N/I/R gate passes. Present-tense
> symptoms, endpoints, and container names below refer to the captured
> runtime, not currently available services.

Cross-cutting symptom-to-diagnostic-step matrix for the captured ManyForge
runtime. Use it as restoration and diagnostic-design input; from each
row jump to the right detailed runbook.

For where to look (log paths, REST endpoints, container names), see
[`OBSERVABILITY.md`](OBSERVABILITY.md). For per-workflow runbooks see
the existing operations docs cross-linked below.

## Quick symptom matrix

| Symptom                                                                        | Most likely cause class                                                                                             | First diagnostic step                                                                                                                   |
| ------------------------------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------- |
| `./scripts/launch.sh start` fails immediately                                  | Image not built, port busy, docker daemon down                                                                      | `./scripts/launch.sh preflight`                                                                                                         |
| `start` returns green but cycle won't run                                      | External dependency (Isaac, vLLM, ros2_control) still warming up                                                    | `./scripts/launch.sh smoke` (blocks on external readiness); see [`LAUNCHER_QUICKSTART.md`](LAUNCHER_QUICKSTART.md) §"first-run gotchas" |
| Composer never reaches `/api/health = ok`                                      | Composer cold C++/RoboPlan build (~10 min first launch); container died                                             | `docker logs manyforge-e2e-composer`; raise `COMPOSER_READY_TIMEOUT_S`                                                                  |
| Cycle runs but tree does not progress                                          | Image-mode demo on experimental path; wrong model selection                                                         | See [`STACK_SETUP.md`](STACK_SETUP.md) §Troubleshooting items 1-2                                                                       |
| Alarm is shown immediately after loading a scene                               | Required provider is genuinely unavailable, or scene initialization failed; ordinary cutover waiting must not alarm | See [§Alarm and recovery behavior](#alarm-and-recovery-behavior)                                                                        |
| Alarm remains after Isaac/provider returns                                     | Condition returned but its activation still requires acknowledgement                                                | Inspect `lifecycleState` in `GET /api/execution/alarms`; acknowledge the returned activation                                            |
| Retry/Resume/Reset only works on the second press                              | Stale recovery offer/presentation or a lifecycle command still pending; this is not expected UX                     | Save cycle status, alarm snapshot, and execution history; see [§Alarm and recovery behavior](#alarm-and-recovery-behavior)              |
| "Collision validation rejected trajectory"                                     | Stale pose source, scene drift, or genuine collision                                                                | See [§Collision rejection](#collision-rejection) below                                                                                  |
| "Motion execution ended in interrupted"                                        | Kernel-native interrupt latched (collision fault, signal source)                                                    | See [§Motion ended interrupted](#motion-ended-interrupted) below                                                                        |
| AI tab disabled or chat request fails | No `ai_model` component / assistant profile, model server down, token variable unset | See [§AI assistant](#ai-assistant) below |
| Isaac lane: never out of "waiting for FJT action / `/joint_states`"            | Isaac not running, controller config not spawned, DDS isolation                                                     | See [§ros2_control discovery failing](#ros2_control-discovery-failing) below                                                            |
| Floating / disconnected URDF mesh pieces                                       | Compound `<origin rpy=…>` on `<visual>` block                                                                       | See [`ADDING_A_ROBOT.md`](ADDING_A_ROBOT.md) §Troubleshooting "Floating / disconnected mesh pieces"                                     |
| Mesh 404 in browser network tab                                                | OBJ missing or `trimesh.load` failed                                                                                | See [`ADDING_A_ROBOT.md`](ADDING_A_ROBOT.md) §"404 on a mesh URL"                                                                       |
| Composer scene mesh shows `missing` / `parse_failed` / `outside_allowed_roots` | Path resolution mismatch / malformed OBJ / allowlist                                                                | See [`ISAAC_SCENE_AUTHORING.md`](ISAAC_SCENE_AUTHORING.md) §Troubleshooting table                                                       |
| Sandbox agent can't reach ManyForge                                            | OpenClaw provider not configured on host                                                                            | See [`STACK_SETUP.md`](STACK_SETUP.md) §"Sandbox agent cannot reach ManyForge cleanly"                                                  |
| Unified memory not freed after `stop` on Thor                                  | `drop_caches` needs sudo                                                                                            | See [`LAUNCHER_QUICKSTART.md`](LAUNCHER_QUICKSTART.md) §"Unified memory not freed"                                                      |

## Detailed flows for session-known failures

### Alarm and recovery behavior

Use these three surfaces together:

1. `GET /api/execution/alarms` — current conditions, lifecycle state, source
   quality, and acknowledgement identity.
1. `GET /api/cycle/status` — current `recoveryOffer`, including exact action
   ids/revisions and whether confirmation or reset is required.
1. `GET /api/execution/history?limit=200` — immutable condition, alarm,
   recovery, and lifecycle events.

Interpret them as follows:

- `active_unacknowledged`: cause is present and operator attention is pending.
- `active_acknowledged`: operator saw it, but the cause remains active.
- `returned_unacknowledged`: the source proved return-to-normal; the occurrence
  still needs acknowledgement.
- `returned_acknowledged`: complete history; no current attention popup.

Stopping Isaac or another required provider after initialization should set a
provider-unavailable condition. Restarting a healthy stream should return the
condition, but should not silently invent operator acknowledgement. By
contrast, a normal program/deployment cutover closes the generation-fenced
scene barrier while reset/seed runs; that expected waiting must not create
pose-apply alarms. Inspect `sceneInitialization` in the load response and
`GET /api/infra/heartbeat`. Report a regression if an ordinary successful
cutover emits `Observed pose could not be applied` before the barrier opens.

One operator press should be sufficient for an enabled action. The server
synchronously resolves the latest recovery offer; a stale request is rejected
without side effects and returns the new offer for display. If the same action
requires a second press, capture both request responses plus the three surfaces
above. `acknowledge_and_continue` is the intentional exception in workflow,
not implementation: it is a non-moving action that leaves the cycle held, so a
separate later Run/Resume press is required to plan and move.

### Collision rejection

**Symptom**: an `ErrorDTO` with code `kErrorValidationCollision` and message
`"Collision validation rejected trajectory"` surfaces through the cycle digest
or execution history. The same category also covers cancellation or an
inconclusive fail-closed query; use the stable message/reason and `details`
fields to distinguish them. Collision details include the canonical pair, body
ids, and collision-world revision when available.

**Where to look**:

1. `GET /api/execution/history?eventTypes=kernel_error&limit=20` — get
   the `details` map for the rejection (canonical pair, body ids,
   `collision_world_revision`).
1. `GET /api/kernel/snapshot` — confirm the active world revision and
   read the structured error.
1. `GET /api/scene/diagnostics` — compare backend-authoritative scene
   state with native viewer render state. **First place to look on the Isaac
   lane**: a stale Isaac pose can drive a rejection even when the
   viewer looks correct.
1. `GET /api/kernel/events?since=N` — scan for interrupt-family events
   that may have cancelled validation under load.

**Common causes**:

- Stale pose source after a scene edit (Isaac lane especially).
  Confirm the object's `policies.scene_pose_sources` row `apply_policy`
  in the deployment — Isaac
  scenes typically use `continuous_unattached` or `before_planning`
  (see [`DEVELOPMENT_WORKBENCH.md`](DEVELOPMENT_WORKBENCH.md)).
- Genuine collision the operator did not anticipate. Use
  `GET /api/scene/snapshot` to visualize the world at the rejection
  revision.
- Validation cancelled by an interrupt latch (message will read
  `"cancelled by interrupt"`, not `"rejected"`). Follow up with
  [§Motion ended interrupted](#motion-ended-interrupted).

**Adjacent reference**: the collision contract and the engine's
projection rules are documented in
[`../reference/ROBOPLAN_ADAPTER_NOTES.md`](../reference/ROBOPLAN_ADAPTER_NOTES.md).

### Motion ended interrupted

**Symptom**: the literal string `Motion execution ended in interrupted` appears
in cycle status, chat overlay, or the behavior result. This is a terminal class,
not the underlying diagnosis; the structured snapshot and same-span events own
the cause.

**Where to look**:

1. `GET /api/kernel/snapshot` — `snapshot.error` (the structured cause
   `motion_runtime.py:93-94` prefers) is the authoritative diagnosis.
   If absent, the f-string fallback was used and the underlying cause
   is in the events.
1. `GET /api/kernel/events?since=N` — filter for the interrupt-family
   event types (`interrupt_signal_ingested`, `interrupt_latched`,
   `interrupt_suppressed_by_active_mask`,
   `interrupt_recovery_handler_started`,
   `interrupt_recovery_handler_completed`,
   `interrupt_ingest_overflow`, `interrupt_source_class_violation`).
   The latched source identifies the cause class.
1. If the latched source is `runtime_collision_fault_source_definition`,
   the immediate cause is a runtime collision fault. Follow up with
   [§Collision rejection](#collision-rejection) — the kernel emits a
   collision rejection through the interrupt path.

**Common causes**:

- Runtime collision fault during execution.
- Cooperative-interrupt removal artifacts: a node still issuing
  cooperative-style cancellation that the new latch model rejects as
  a class violation. Check `interrupt_source_class_violation` events.
- Stop issued from another surface (Composer UI, MCP tool,
  bringup script) — the launcher's `status` verb shows which surface
  is connected.

**Adjacent reference**: use the generated REST contract and execution-history
event payloads; source locations are implementation detail and may move.

### AI assistant

**Symptom**: the AI tab says "No AI model component in this Deployment", or a
chat request fails with a typed code.

**Cause and step**: the tab needs an assistant profile and an `ai_model`
component for its chat alias in the active Deployment; requests need the model
server to serve the component's `model` and, when `api_key_env` is set, that
`MANYFORGE_AI_*` variable exported before starting the workbench. Check `GET /api/health`
(`assistant_bridge: configured`), `GET /api/assistant/modes`
(`ai_model_component_id`), the `endpoint` component stream, and
`curl {base_url}/models`. Codes are listed in
[`ASSISTANT_E2E_COOKBOOK.md`](ASSISTANT_E2E_COOKBOOK.md) §"Failure codes".

### ros2_control discovery failing

**Symptom**: on the `isaac-sim` lane (`isaac-ur10e-light-scene` or
`isaac-ur10e-light-scene-assistant` scenarios), Composer never
transitions out of "waiting for FJT action server" or "waiting for
`/joint_states`". The deployment declares the FJT action at
`/isaac_arm_controller/follow_joint_trajectory` and
`controller_manager: /controller_manager` with
`wait_for_action_server_timeout_s: 2.0` and
`joint_state_max_age_ms: 250` — once exhausted, Composer flags the
supervisor as `waiting` instead of `ready`.

**Where to look**:

1. `./scripts/demo-isaac-ros2-control.sh status` — supervisor
   readiness JSON (referenced from
   [`STACK_SETUP.md`](STACK_SETUP.md) §"Isaac ros2_control execution").
1. `GET /api/runtime/status` — confirms what the Composer side sees.
1. `GET /api/infra/heartbeat` — what the launcher's `status` verb
   polls.
1. `docker compose logs --tail 200 composer-ros` — Composer's
   stderr will show `"wait_for_action_server timeout"` or `"joint state stale (age >250 ms)"`.

**On the ROS host (outside the container, with `network_mode: host`)**:

```bash
ros2 action list | grep follow_joint_trajectory
ros2 topic hz /joint_states                       # confirm > 4 Hz
ros2 control list_controllers                     # confirm isaac_arm_controller is active
```

**Common causes**:

- Isaac Sim not running, or `topic_based_ros2_control` bridge not
  loaded. `/isaac_joint_states` never publishes.
- Controller config file
  `manyforge_ros_bringup/config/isaac_ur10e_topic_based_controllers.yaml`
  not spawned by the launch file — `controller_manager` name mismatch
  with the deployment's `controller_manager: /controller_manager`.
- DDS discovery isolation. The compose services declare
  `network_mode: host` and `ipc: host`
  (`docker-compose.yaml:104-105,135-136`) — but a non-default
  `ROS_DOMAIN_ID` or DDS profile on the host can break Isaac↔Composer
  visibility. Confirm with `ros2 doctor` and `echo $ROS_DOMAIN_ID`.

**Note on tolerances**: the Isaac preview deployment intentionally
sets `disable_controller_path_tolerances: true`
(`isaac_manipulator_scene_light_ros2_control.deployment.yaml:128`).
Real-robot deployments **must** keep this `false`. If a
controller-state tolerance abort appears on Isaac, that's a deployment
misconfig, not a controller bug. See [`STACK_SETUP.md`](STACK_SETUP.md)
§"Real ROS 2 controller / robot execution".

### Lite6 trocar FoundationPose stream is unstable or missing

Use [`ISAAC_LITE6_TROCAR_FOUNDATIONPOSE.md`](ISAAC_LITE6_TROCAR_FOUNDATIONPOSE.md)
as the primary runbook. The short diagnostic order is:

1. Confirm the DDS environment is set in all three contexts: Isaac Sim laptop,
   Isaac ROS container, and the ManyForge launcher shell.

   ```bash
   echo "$ROS_DOMAIN_ID"
   echo "$RMW_IMPLEMENTATION"
   echo "$ROS_AUTOMATIC_DISCOVERY_RANGE"
   echo "$ROS_STATIC_PEERS"
   ```

   If any value is empty or different from the runbook baseline, source the
   profile before launching that participant:

   ```bash
   MANYFORGE_ROOT="${MANYFORGE_ROOT:-${HOME}/workspaces/dev_ws/src/manyforge}"
   source "${MANYFORGE_ROOT}/config/ros/isaac_lite6_trocar_foundationpose.env.sh"
   ```

1. Check low-payload graph info before subscribing to raw image topics:

   ```bash
   ros2 topic list --no-daemon --spin-time 20 -t | \
     grep -E 'image_rect|depth|camera_info|detections_output|/output'
   ros2 topic info --no-daemon --spin-time 20 -v /output
   ```

1. Echo `/output` with explicit type and QoS:

   ```bash
   ros2 topic echo /output vision_msgs/msg/Detection3DArray --once --timeout 120 \
     --qos-reliability reliable --qos-durability volatile
   ```

1. If ManyForge reports `tf_lookup_failed`, verify:

   ```bash
   ros2 run tf2_ros tf2_echo link_base camera_color_optical_frame
   ```

Known commissioning limitation: the current Isaac ROS FoundationPose `/output`
may publish empty detection id/class, score `0.0`, and zero covariance. The
trocar deployment and smoke validator explicitly allow that only for the
controlled single-trocar bootstrap scene. Treat a production cell with those
quality fields still empty as unqualified.

## Where else troubleshooting lives

Per-workflow troubleshooting blocks already exist in the operations
docs. This matrix is the cross-cutting entry point; the detailed
remediations stay in the workflow docs:

- [`LAUNCHER_QUICKSTART.md`](LAUNCHER_QUICKSTART.md) §"Likely
  first-run gotchas" (lines 344-393) — startup failures, port
  conflicts, timeouts, drop_caches/sudo.
- [`STACK_SETUP.md`](STACK_SETUP.md) §"Troubleshooting"
  (lines 434+) — cycle stuck, model selection ignored, sandbox
  reachability.
- [`ISAAC_SCENE_AUTHORING.md`](ISAAC_SCENE_AUTHORING.md)
  §"Troubleshooting" (lines 253-263) — Isaac mesh inventory, transfer,
  parse, allowlist issues.
- [`ADDING_A_ROBOT.md`](ADDING_A_ROBOT.md) §"Troubleshooting"
  (lines 312-381) — URDF/mesh rendering, mimic chains,
  Robot Control joint-limit errors, frame visibility.
- [`ASSISTANT_E2E_COOKBOOK.md`](ASSISTANT_E2E_COOKBOOK.md) — AI assistant
  setup, checks and failure codes.

## Capturing a failure for later analysis

See [`OBSERVABILITY.md`](OBSERVABILITY.md) §"Capturing a bug report"
for the full capture recipe. The minimum bundle for a
collision/interrupt failure:

```bash
RUN_ID=<from cycle status>
curl -s "http://127.0.0.1:9000/api/debug/export?run_id=${RUN_ID}" > debug-export.json
curl -s "http://127.0.0.1:9000/api/execution/history?run_id=${RUN_ID}&limit=1000" > history.json
curl -s "http://127.0.0.1:9000/api/kernel/snapshot" > kernel-snapshot.json
docker logs --tail 2000 manyforge-e2e-composer > composer.log
```

Attach also the launcher invocation
(`./scripts/launch.sh --lane … --assistant … --scenario …`) and the
deployment's `orchestrator.config.validation_collision_workers` requested and
reported effective values. Also record
`MANYFORGE_VALIDATION_COLLISION_WORKERS` when a direct-factory development/CI
run used that fallback.
