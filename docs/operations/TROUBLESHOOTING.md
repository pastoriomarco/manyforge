# Troubleshooting — Symptom → Cause → Diagnostic Step

Cross-cutting symptom-to-diagnostic-step matrix. From each row, jump to the
detailed section or runbook.

For where to look (logs, REST endpoints, container name), see
[`OBSERVABILITY.md`](OBSERVABILITY.md). REST paths below are on the Composer
API, `http://127.0.0.1:9020`.

## Quick symptom matrix

| Symptom | Most likely cause class | First diagnostic step |
| --- | --- | --- |
| `./scripts/compose-dev.sh start` fails | Image missing, port occupied, Docker daemon down | Read the start output; see the [workbench troubleshooting table](DEVELOPMENT_WORKBENCH.md#operation-and-troubleshooting) |
| `start` succeeds but the cycle won't run | External dependency (Isaac, controllers) still warming up | `./scripts/compose-dev.sh status`: check `"ready"` and `"unavailable_reasons"` |
| Start never reports ready | First start compiles the C++ targets; container died | `docker logs --tail 200 manyforge-development-workbench`; lower `MANYFORGE_DEV_BUILD_JOBS` if compilation exhausts memory |
| Cycle runs but tree does not progress | Held cycle, pending recovery offer, active alarm | See [`STACK_SETUP.md`](STACK_SETUP.md) §Troubleshooting |
| Alarm remains after Isaac/provider returns | Condition returned but its activation still requires acknowledgement | Inspect `lifecycle_state` in `GET /api/execution/alarms`; acknowledge the returned activation |
| Retry/Resume/Reset only works on the second press | Stale recovery offer or a lifecycle command still pending; this is not expected UX | Save cycle status, alarm snapshot, and execution history; see [§Alarm and recovery behavior](#alarm-and-recovery-behavior) |
| Motion refused with `validation_collision` | Stale pose source, scene drift, or genuine collision | See [§Collision rejection](#collision-rejection) below |
| Motion stopped with **Robot position feedback lost** (`runtime_monitor_observation_lost`) | The robot's measured joint states stopped arriving or became too old during motion, typically because the robot controller faulted or its connection dropped; nothing was detected in contact | Inspect the operation alarm in Runtime and its History entry; check the robot component status and controller connection before using Recover as offered |
| AI tab disabled or chat request fails | No `ai_model` component / assistant profile, model server down, token variable unset | See [§AI assistant](#ai-assistant) below |
| Isaac lane never becomes ready | Isaac not playing, controller not active, DDS isolation | See [§Isaac controller discovery failing](#isaac-controller-discovery-failing) below |
| Composer scene mesh shows `missing` / `parse_failed` / `outside_allowed_roots` | Path resolution mismatch / malformed OBJ / allowlist | See [`ISAAC_SCENE_AUTHORING.md`](ISAAC_SCENE_AUTHORING.md) §Troubleshooting table |

## Detailed flows

### Alarm and recovery behavior

Use these three surfaces together:

1. `GET /api/execution/alarms` — current conditions, lifecycle state, source
   quality, and acknowledgement identity.
1. `GET /api/cycle/status` — current `recovery_offer`, including exact action
   ids/revisions and whether confirmation or reset is required.
1. `GET /api/execution/history` — the latest 32 condition, alarm, recovery,
   and lifecycle events (page further with `before_sequence`).

An alarm's `lifecycle_state` is one of:

- `active_unacknowledged`: cause is present and operator attention is pending.
- `active_acknowledged`: operator saw it, but the cause remains active.
- `returned_unacknowledged`: the source proved return-to-normal; the occurrence
  still needs acknowledgement.
- `returned_acknowledged`: complete history; no current attention popup.

Stopping Isaac or another required provider after initialization sets a
provider-unavailable condition. Restarting a healthy stream returns the
condition, but does not acknowledge it for the operator.

One operator press should be sufficient for an enabled action. A stale request
is rejected without side effects. If the same action requires a second press,
capture both request responses plus the three surfaces above.
`acknowledge_and_continue` is a non-moving action that leaves the cycle held,
so a separate later Run/Resume press is required to plan and move.

### Collision rejection

**Symptom**: a motion is refused with the typed code `validation_collision`.
The same code also covers cancellation or an inconclusive fail-closed query;
use the message and `details` to distinguish them. Collision details include
the colliding pair and body ids when available. `details.classification:
clearance` means the trajectory passes closer than the move's collision
clearance (`collision_clearance_m`) to that pair without touching it
(`signed_clearance_m`); the planner retries a detour first. A goal (or start)
inside the clearance is refused by the planner (`planning_failed`,
`native_goal_state_collision`, `classification: clearance`). For deliberate
close work, set a smaller `clearance_m` on that motion node or in Robot
Control's Advanced settings, or lower the Deployment `collision_clearance_m`.

**Where to look**:

1. Runtime/history in Composer, or `GET /api/execution/history` — the
   refusal event and its details.
1. `GET /api/scene` — the backend-authoritative scene the planner used.
   **First place to look on the Isaac lane**: a stale Isaac pose can drive a
   rejection even when the viewer looks correct. Enable **Measured state
   ghosts** in the viewer to compare observed and planning poses.

**Common causes**:

- Stale pose source after a scene edit (Isaac lane especially). Check the
  object's `apply_policy` in the Deployment's `policies.scene_pose_sources`
  (see [`ISAAC_SCENE_AUTHORING.md`](ISAAC_SCENE_AUTHORING.md) §4).
- Genuine collision the operator did not anticipate.

### AI assistant

**Symptom**: the AI tab says "No AI model component in this Deployment", or a
chat request fails with a typed code.

**Cause and step**: the tab needs an assistant profile and an `ai_model`
component for its chat alias in the active Deployment; requests need the model
server to serve the component's `model` and, when `api_key_env` is set, that
`MANYFORGE_AI_*` variable exported before starting the workbench. Check
`GET /api/assistant/modes` (`ai_model_component_id`), the `endpoint` component
stream, and `curl {base_url}/models`. Codes are listed in
[`ASSISTANT_E2E_COOKBOOK.md`](ASSISTANT_E2E_COOKBOOK.md) §"Failure codes".

### Isaac controller discovery failing

**Symptom**: on an Isaac Scenario (for example
`examples/isaac-ur10e-light-scene.scenario`), `status` never reports ready, or
the arm component stays unavailable.

**Where to look**:

1. `./scripts/compose-dev.sh status` — readiness and
   `unavailable_reasons` of the current Scenario.
1. Composer's component status, or `GET /api/component-overview`.
1. `docker logs --tail 200 manyforge-development-workbench`.

**ROS graph checks**, run inside the workbench container (the image entrypoint
sources ROS):

```bash
W=manyforge-development-workbench
docker exec $W docker-entrypoint.ros-jazzy-full.sh ros2 topic list
docker exec $W docker-entrypoint.ros-jazzy-full.sh ros2 control list_controllers
docker exec $W docker-entrypoint.ros-jazzy-full.sh ros2 action list
```

**Common causes**:

- Isaac Sim not playing, so `/isaac_joint_states` (the topic named in the
  Deployment) never publishes.
- Controller not active; the Isaac UR10e controllers are defined in
  `manyforge_ros_bringup/config/isaac_ur10e_topic_based_controllers.yaml`.
- DDS discovery isolation: a different `ROS_DOMAIN_ID`, RMW or DDS profile in
  Isaac Sim and in the shell that ran `./scripts/compose-dev.sh start`.

### Lite6 trocar FoundationPose stream is unstable or missing

Use [`ISAAC_LITE6_TROCAR_FOUNDATIONPOSE.md`](ISAAC_LITE6_TROCAR_FOUNDATIONPOSE.md)
as the primary runbook. The short diagnostic order is:

1. Confirm the DDS environment is set in all three contexts: Isaac Sim host,
   Isaac ROS container, and the shell that runs `./scripts/compose-dev.sh start`.

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

Known commissioning limitation: the Isaac ROS FoundationPose `/output` may
publish empty detection id/class, score `0.0`, and zero covariance. The trocar
deployment allows that only for the controlled single-trocar scene. Treat a
production cell with those quality fields still empty as unqualified.

## Where else troubleshooting lives

- [`DEVELOPMENT_WORKBENCH.md`](DEVELOPMENT_WORKBENCH.md)
  §"Operation and troubleshooting" — startup failures, port
  conflicts, slow first start, stale pages.
- [`STACK_SETUP.md`](STACK_SETUP.md) §"Troubleshooting" — cycle stuck, model
  selection ignored.
- [`ISAAC_SCENE_AUTHORING.md`](ISAAC_SCENE_AUTHORING.md)
  §"Troubleshooting" — Isaac mesh inventory, transfer, parse, allowlist issues.
- [`ASSISTANT_E2E_COOKBOOK.md`](ASSISTANT_E2E_COOKBOOK.md) — AI assistant
  setup, checks and failure codes.

## Capturing a failure for later analysis

See [`OBSERVABILITY.md`](OBSERVABILITY.md) §"Capturing a bug report"
for the full capture recipe. The minimum bundle:

```bash
RUN_ID=<program_run_id from Runtime/history>
API=http://127.0.0.1:9020
curl -s "$API/api/debug/export?run_id=${RUN_ID}" > debug-export.json
curl -s "$API/api/execution/history?program_run_id=${RUN_ID}" > history.json
curl -s "$API/api/execution/alarms" > alarms.json
curl -s "$API/api/cycle/status" > cycle-status.json
docker logs --tail 2000 manyforge-development-workbench > workbench.log
```

Attach also the start command (`./scripts/compose-dev.sh start --project …`)
and the `./scripts/compose-dev.sh status` output.
