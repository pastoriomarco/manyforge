> Docs entrypoint: [`../INDEX.md`](../INDEX.md)

# Isaac Sim Preview Plan

This is the tactical execution plan for the first simulator-backed ManyForge
preview.

It is not a peer strategic plan. It is a sub-plan of:

- [`PREVIEW_AND_ADOPTION_PLAN.md`](PREVIEW_AND_ADOPTION_PLAN.md)

It should also remain aligned with:

- [`PRODUCT_DIRECTION_PLAN.md`](PRODUCT_DIRECTION_PLAN.md)
- [`INDUSTRIAL_EXECUTION_READINESS_PLAN.md`](INDUSTRIAL_EXECUTION_READINESS_PLAN.md)

Technical reference for the simulator boundary:

- [`../reference/SIMULATOR_ADAPTER_CONTRACT_DRAFT.md`](../reference/SIMULATOR_ADAPTER_CONTRACT_DRAFT.md)

## Purpose

Use Isaac Sim as the first serious simulator-backed implementation of the
ManyForge preview/adoption story.

This does not mean Isaac-specific lock-in is the long-term target. It means:

- Isaac is the first concrete simulator path to validate the workflow
- the resulting design should remain compatible with a broader simulation
  interface strategy

## Goal

Reach a presentable simulator-backed ManyForge workflow that shows:

- a loaded Isaac Sim scene
- ManyForge reading the relevant live scene state
- ManyForge reproducing the relevant scene/resources in its own backend/HMI
- assistant-backed generation of a usable ManyForge program
- execution of that program against the simulated robot and scene

## Architectural Constraints

The Isaac path must not:

- bypass ManyForge runtime authority
- create simulator-only program semantics
- depend on assistant-side uncontrolled mutation
- become a dead-end adapter stack that cannot inform broader simulator support

## Open Integration Decision

The simulator control and scene path is still open.

The current reference boundary for comparing those options is:

- [`../reference/SIMULATOR_ADAPTER_CONTRACT_DRAFT.md`](../reference/SIMULATOR_ADAPTER_CONTRACT_DRAFT.md)

Candidate implementation paths:

### 1. Direct Isaac adapter path

Pros:

- avoids taking on ROS wrappers immediately
- can be narrower for the first implementation

Cons:

- may be more simulator-specific
- can drift if not designed carefully against a broader abstraction

### 2. ROS 2 wrapper path

Pros:

- aligns with existing ROS-oriented simulation ecosystems
- can potentially leverage cross-simulator standards such as
  `simulation_interfaces`

Cons:

- may pull ROS-wrapper work into the short-term scope earlier than desired
- may be broader than needed for the first proof

### 3. Hybrid path

Pros:

- allows direct work for narrow needs while keeping a future ROS path open

Cons:

- easiest way to create accidental complexity if boundaries are not explicit

Current decision:

- keep all three options open until the next implementation discussion
- explicitly treat ROS 2 `simulation_interfaces` as a strong candidate for the
  broader simulation path
- do not yet commit the preview implementation to the ROS-wrapper route

## Discovery Checkpoint: Isaac ROS And RoboPlan ROS

Date: 2026-04-24

This checkpoint records the current live Isaac Sim / Isaac ROS discovery
results. It should be treated as spike evidence, not as a final architecture.

### Isaac ROS Container Notes

Current setup:

- Isaac Sim is running on a separate LAN machine and publishing ROS 2 data.
- The Jetson-side Isaac ROS container is accessed with `isaac-ros activate` or,
  for non-interactive commands, with `docker exec isaac_ros_dev_container ...`.
- The container is disposable; package installs performed inside it are not
  persistent across container recreation.
- The ROS distro in the container is Jazzy.

`simulation_interfaces` was not usable in the base container until the package
and a matching Fast DDS / Fast CDR stack were installed or upgraded.

Install sequence used in the disposable container:

```bash
source /opt/ros/jazzy/setup.bash
sudo apt update
sudo apt install -y --no-install-recommends ros-jazzy-simulation-interfaces
```

After installing only `ros-jazzy-simulation-interfaces`, ROS 2 CLI calls failed
with a Fast CDR / Fast DDS symbol mismatch:

```text
undefined symbol: _ZN8eprosima7fastcdr3Cdr9serializeERKh
```

The mismatch was resolved by upgrading the relevant Fast DDS / Fast CDR and
FastRTPS type-support packages:

```bash
sudo apt install -y --only-upgrade \
  ros-jazzy-fastcdr \
  ros-jazzy-fastrtps \
  ros-jazzy-rmw-fastrtps-cpp \
  ros-jazzy-rmw-fastrtps-shared-cpp \
  ros-jazzy-rosidl-typesupport-fastrtps-c \
  ros-jazzy-rosidl-typesupport-fastrtps-cpp \
  ros-jazzy-rosidl-dynamic-typesupport-fastrtps
```

Validation commands:

```bash
source /opt/ros/jazzy/setup.bash
ros2 interface show simulation_interfaces/srv/GetSimulatorFeatures
ros2 service list -t | grep simulation_interfaces
ros2 service call /get_simulator_features \
  simulation_interfaces/srv/GetSimulatorFeatures '{}'
```

Important naming detail:

- NVIDIA documentation commonly shows names such as
  `/isaacsim/GetEntityState`.
- The currently observed scene exposes root-level snake-case names such as
  `/get_entity_state`, `/get_entities_states`, and
  `/get_simulator_features`.

Adapters must discover service names or make them configurable; they must not
hard-code one naming convention.

### Current Live ROS Graph Findings

Observed `simulation_interfaces` services:

- `/get_simulator_features`
- `/get_current_world`
- `/get_available_worlds`
- `/get_entities`
- `/get_entity_info`
- `/get_entity_state`
- `/get_entities_states`
- `/set_entity_state`
- `/spawn_entity`
- `/delete_entity`
- `/reset_simulation`
- `/set_simulation_state`
- `/get_simulation_state`
- `/step_simulation`
- `/load_world`
- `/unload_world`

Observed simulation action:

- `/simulate_steps` using `simulation_interfaces/action/SimulateSteps`

Observed robot/gripper topics:

- `/isaac_joint_states` as `sensor_msgs/msg/JointState`
- `/isaac_joint_commands` as `sensor_msgs/msg/JointState`
- `/isaac_gripper_state` as `sensor_msgs/msg/JointState`

Important runtime facts:

- `/isaac_joint_states` has been observed between roughly 6.5 Hz and 14 Hz
  during this discovery session, depending on scene/load state.
- `/isaac_joint_commands` has one Isaac Sim subscriber and no publishers.
- `/isaac_gripper_state` also appears to be a command topic despite the
  `state` suffix.
- Before adding the temporary bridge, no
  `control_msgs/action/FollowJointTrajectory` action server was exposed by the
  scene.
- Before adding the temporary bridge, no `controller_manager` services were
  exposed by the scene.
- `simulation_interfaces` gives a usable world/entity path, but it does not
  solve robot trajectory execution by itself.

Observed world/entity behavior:

- The active world can be queried.
- Entity listing works but returns a noisy full simulator graph, so ManyForge
  will need filtering and explicit binding rules.
- Entity state queries can return live poses for graspable objects and table
  elements.
- `GetEntityInfo` currently returns weak semantic metadata in this scene
  (`category` and `tags` are not useful enough for automatic classification).

### manymove Comparison

manymove uses a split approach in similar Isaac setups:

- `simulation_interfaces` is used for simulator entity pose get/set.
- Robot arm execution still goes through MoveIt / ros2_control-style
  trajectory execution.
- The Isaac-specific gripper route uses raw `sensor_msgs/msg/JointState`
  publishing.

Relevant manymove evidence:

- `GetEntityPoseNode` / `SetEntityPoseNode` call
  `simulation_interfaces` services for simulator entities.
- `PublishJointStateAction` publishes raw joint-state commands for the Isaac
  gripper path.
- The planner execution path sends `control_msgs/action/FollowJointTrajectory`
  goals to a controller and monitors feedback/result.
- The Isaac launch path still brings up MoveGroup, controller manager, and
  controller spawners; it is not a pure direct-Isaac execution path.

Conclusion:

- manymove confirms that `simulation_interfaces` is a good environment/entity
  path.
- manymove does not prove that `simulation_interfaces` is sufficient for robot
  execution.
- It also does not prove that MoveIt is required when RoboPlan is already the
  planner; it mainly proves that a standard trajectory execution surface is
  needed.

### RoboPlan / RoboPlan ROS Upstream Findings

Upstream repositories inspected:

- <https://github.com/open-planning/roboplan>
- <https://github.com/open-planning/roboplan-ros>

Related external references consulted:

- <https://docs.isaacsim.omniverse.nvidia.com/5.0.0/ros2_tutorials/tutorial_ros2_simulation_control.html>
- <https://nvidia-isaac-ros.github.io/v/release-4.0/reference_workflows/isaac_for_manipulation/tutorials/isaac_sim/tutorial_isaac_sim_pick_and_place.html>
- <https://control.ros.org/iron/doc/ros2_controllers/joint_trajectory_controller/doc/userdoc.html>

RoboPlan core position:

- RoboPlan is a standalone motion planning library.
- Middleware is intentionally optional and belongs in wrappers.
- Core trajectory data already contains joint names, times, positions,
  velocities, and accelerations.

RoboPlan ROS position:

- `roboplan-ros` is intentionally a thin wrapper repository.
- It prefers standard ROS messages such as `sensor_msgs/JointState` and
  `trajectory_msgs/JointTrajectory`.
- It explicitly avoids providing monolithic generic ROS nodes; complete nodes
  are expected to live in examples or application code.

Concrete useful pieces:

- `roboplan_ros_cpp` converts `roboplan::JointTrajectory` to and from
  `trajectory_msgs/msg/JointTrajectory`.
- `roboplan_ros_cpp` converts ROS joint state data into RoboPlan joint
  configurations.
- The Franka example uses `controller_manager`, `joint_state_broadcaster`, and
  `joint_trajectory_controller/JointTrajectoryController`.

Concrete limitation:

- `roboplan-ros` does not currently provide a drop-in execution backend,
  action server, or Isaac adapter that ManyForge can reuse directly.
- It does support the direction of converting RoboPlan trajectories to
  standard ROS trajectory messages.

Implication for ManyForge:

- A ManyForge ROS execution backend should target standard ROS trajectory
  execution first, especially `FollowJointTrajectory`.
- RoboPlan trajectory conversion logic can inform or be reused by that backend,
  but ManyForge still owns execution semantics, stop/pause/resume/reset, trace
  events, and industrial validation.

### Current Route Decisions

The original route options have now been narrowed by the discovery spikes.

#### Route A: direct Isaac topics

Status: proven as a diagnostic/smoke-test path, not selected as the durable
execution interface.

Evidence:

- `/isaac_joint_states` can be observed from the ROS lane.
- `/isaac_joint_commands` and `/isaac_gripper_state` have Isaac Sim
  subscribers.
- Tiny bounded commands can move the simulated arm and gripper.

Reason for not productizing this route:

- raw topic publishing has weak result, tolerance, cancel, controller lifecycle,
  and stop semantics
- it is simulator-specific
- industrial execution should not depend on fire-and-forget joint-state command
  topics

Keep this route only for diagnostics, command-surface discovery, and emergency
comparison when debugging the standard execution path.

#### Route B: ros2_control / `FollowJointTrajectory`

Status: selected as the leading execution target, but not yet reproducible in
the new `manyforge-ros-jazzy` lane.

Evidence:

- the route was smoke-tested against the current Isaac scene by rebuilding
  `topic_based_ros2_control` from source in the disposable container
- it exposed a standard
  `/isaac_arm_controller/follow_joint_trajectory` action
- tiny action goals were accepted, completed, and moved the robot through the
  existing Isaac joint command/state topics

Reason for selecting this route:

- aligns with RoboPlan ROS trajectory conversion direction
- aligns with real-robot ROS controller patterns
- avoids making MoveIt mandatory for execution when RoboPlan is the planner
- gives ManyForge a standard surface for feedback, result, cancel, tolerance,
  timeout, and trace semantics

Remaining work:

- make the bridge reproducible in `manyforge-ros-jazzy`
- validate controller lifecycle, cancel, stop, timeout, tolerance, final-state
  accuracy, and error propagation
- decide whether `topic_based_ros2_control` is a dependency to vendor, build
  from source, or replace with a ManyForge-owned bridge

#### Route C: MoveIt / cuMotion workflow

Status: reference/interoperability path only.

Keep this route for comparison with Isaac ROS reference workflows and for
capabilities ManyForge explicitly decides not to own. Do not put MoveIt in the
critical path merely to execute RoboPlan trajectories.

#### Route D: direct Isaac Python/extension adapter

Status: rejected as the durable robot/control adapter; accepted as a narrow
read-only collider-introspection bridge.

Reason for rejecting it as the main adapter:

- poor fit for the two-machine deployment
- likely recreates transport, discovery, and execution semantics that ROS 2
  already provides
- weak portability to real robot control
- high throwaway risk

Reason for accepting the narrow service:

- `simulation_interfaces` does not expose the collider and bounds data needed
  for practical ManyForge scene reconstruction
- Isaac Python can inspect USD/PhysX collider metadata from the running scene
- the extension still publishes over ROS 2, so Thor-side ManyForge tooling can
  remain in the ROS-capable adapter lane rather than using manual file exchange

### Current Recommendation

Proceed with:

1. Read-only scene mirror in `manyforge_ros`.
   Query `simulation_interfaces`, filter useful entities, and produce a
   ManyForge-friendly scene snapshot.
2. Isaac collider service extension for authoring geometry.
   Use Isaac Python/PhysX introspection only for read-only collider inventory
   that is missing from `simulation_interfaces`, and expose it through ROS 2 so
   Thor-side ManyForge tools can request it on demand.
3. Reproducible ros2_control bridge in `manyforge-ros-jazzy`.
   Expose `FollowJointTrajectory` over the current Isaac joint command/state
   topics.
4. ManyForge ROS execution adapter against `FollowJointTrajectory`.
   This becomes the durable execution surface, not raw Isaac topics.
5. Industrial validation of execution semantics.
   Validate cancel/stop timing, timeout behavior, tolerances, final-state
   checks, trace events, and failure propagation before treating the route as
   productizable.

### Direct Topic Smoke Test Result

Date: 2026-04-24

The first direct-topic smoke test was executed against the live Isaac Sim scene.

Test constraints:

- simulation only
- robot in a user-confirmed safe pose
- no broad motion
- no-op command first
- tiny reversible command second
- observed through `/isaac_joint_states`

Arm command test:

- Command topic: `/isaac_joint_commands`
- State topic: `/isaac_joint_states`
- Message type: `sensor_msgs/msg/JointState`
- Commanded joints:
  - `shoulder_pan_joint`
  - `shoulder_lift_joint`
  - `elbow_joint`
  - `wrist_1_joint`
  - `wrist_2_joint`
  - `wrist_3_joint`
- Commanded motion: `wrist_3_joint +0.02 rad`
- Observed `wrist_3_joint` movement: about `+0.019 rad`
- Return command brought `wrist_3_joint` back within about `0.0001 rad` of
  the captured baseline.

Arm caveat:

- A no-op current-state command still coincided with small shoulder/elbow
  settling or drift, up to about `0.0055 rad`.
- This must be measured as controller/physics behavior before using direct
  topic execution for anything beyond smoke testing.

Gripper command test:

- Command topic: `/isaac_gripper_state`
- State topic: `/isaac_joint_states`
- Message type: `sensor_msgs/msg/JointState`
- Commanded joint: `finger_joint`
- Commanded motion: `finger_joint +0.01 rad`
- Observed `finger_joint` movement: about `+0.0009 rad`

Gripper caveat:

- The gripper topic is connected and has observable effect, but this tiny
  command did not track the requested target closely.
- The gripper path needs separate calibration for accepted joint names,
  position scaling, effort fields, command duration, and expected feedback
  semantics before it can be treated as reliable.

Smoke-test conclusion:

- The current Isaac scene can be commanded without MoveIt through raw ROS 2
  topics.
- This confirms that a RoboPlan-to-Isaac proof is technically possible without
  MoveIt.
- The direct topic path remains insufficient as a durable industrial execution
  interface because it lacks action-level result, tolerance, cancel, and
  controller lifecycle semantics.
- The next step should be the ros2_control bridge route so ManyForge can target
  `FollowJointTrajectory` rather than simulator-specific raw joint-state
  commands.

### ros2_control Bridge Smoke Test Result

Date: 2026-04-24

The second route was tested by bridging the existing Isaac topics into
ros2_control and exposing a standard
`control_msgs/action/FollowJointTrajectory` interface.

Packages installed in the disposable Isaac ROS container:

```bash
sudo apt update
sudo apt install -y --no-install-recommends \
  ros-jazzy-topic-based-ros2-control \
  ros-jazzy-joint-trajectory-controller \
  ros-jazzy-joint-state-broadcaster \
  ros-jazzy-controller-manager \
  ros-jazzy-control-msgs \
  ros-jazzy-ros2-control \
  ros-jazzy-ros2-controllers
```

Important compatibility finding:

- The binary `ros-jazzy-topic-based-ros2-control 0.3.0-0noble` crashed during
  ros2_control hardware initialization with the current installed
  `ros-jazzy-controller-manager 4.44.0`.
- Rebuilding the locally mounted `topic_based_ros2_control` source package
  against the active container ROS stack fixed the crash.
- The build emitted a deprecation warning for the older
  `on_init(const HardwareInfo&)` API, so this bridge package should be treated
  as compatible enough for the spike, not as a locked production dependency.

Source build command used:

```bash
source /opt/ros/jazzy/setup.bash
cd /workspaces/isaac_ros-dev
colcon build --packages-select topic_based_ros2_control \
  --symlink-install \
  --cmake-args -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF
```

Bridge configuration:

- A minimal six-joint ros2_control URDF was generated for:
  - `shoulder_pan_joint`
  - `shoulder_lift_joint`
  - `elbow_joint`
  - `wrist_1_joint`
  - `wrist_2_joint`
  - `wrist_3_joint`
- `topic_based_ros2_control/TopicBasedSystem` was configured with:
  - `joint_commands_topic: /isaac_joint_commands`
  - `joint_states_topic: /isaac_joint_states`
- `joint_state_broadcaster` and `joint_trajectory_controller` were spawned.

Resulting ROS control surface:

- Active controller:
  `/isaac_arm_controller`
- Standard action:
  `/isaac_arm_controller/follow_joint_trajectory`
- Claimed command interfaces:
  six arm `position` interfaces
- State interfaces:
  six arm `position` and `velocity` interfaces

Action smoke test:

- Sent a tiny `FollowJointTrajectory` goal moving `wrist_3_joint` by
  `+0.02 rad`.
- The controller accepted the goal and returned success.
- Observed `wrist_3_joint` movement during the first sample after completion:
  about `+0.0139 rad`.
- Sent a return-to-baseline trajectory.
- The controller accepted the return goal and returned success.
- A later state sample showed `wrist_3_joint` back within about `0.0001 rad` of
  the original baseline.

Bridge caveats:

- The action result was produced under loose spike tolerances, so it is not yet
  evidence of production-grade tracking precision.
- The first post-result sample was not always the final settled state.
- Controller tolerance, timing, state rate, goal-time behavior, cancellation,
  stop behavior, and error propagation still need explicit validation.
- The source-built bridge requirement must be converted into a reproducible
  container image or workspace dependency before this route can be considered
  stable.

Bridge conclusion:

- Isaac Sim can be commanded without MoveIt through a standard
  `FollowJointTrajectory` action by using ros2_control over the existing Isaac
  joint command/state topics.
- This is the strongest current route for executing RoboPlan trajectories in
  Isaac while avoiding a MoveIt runtime dependency.
- The next technical step is to turn this spike into a repeatable bridge
  package/config and validate full trajectory semantics, especially stop,
  cancel, timeout, tolerances, and gripper control.

### Containerization And ROS Test Lane Decision

Date: 2026-04-24

The ManyForge container strategy should separate core testing, generic ROS
adapter validation, and Isaac ROS deployment overlays.

Target image families:

1. `manyforge-core`
   - current Debian-based image
   - ROS-free by design
   - used for core/kernel, behavior, Composer, RoboPlan, frontend, and fast
     regression tests
   - keeps ROS/DDS/Isaac dependencies from polluting core correctness

2. `manyforge-ros-jazzy`
   - official ROS Jazzy Noble base image (`ros:jazzy-ros-base-noble`)
   - primary development and validation lane for ROS wrappers/adapters
   - keeps Python/ROS dependencies apt-managed; it must not pip-install or
     override the ROS/colcon interpreter stack
   - contains ROS packages required for:
     - `FollowJointTrajectory`
     - `simulation_interfaces`
     - ros2_control/controller-manager tests
     - fake ROS graph targets
   - runs `manyforge_ros` adapter tests against the mounted workspace
   - should be able to talk to Isaac ROS, Isaac Sim, Gazebo, MoveIt, or real
     robot ROS drivers without becoming Isaac-specific

3. `manyforge-isaac-ros`
   - downstream deployment/convenience image based on NVIDIA Isaac ROS
   - target for Thor setups that need Isaac ROS perception and Isaac Sim
     workflows
   - must reuse the same `manyforge_ros` adapter code instead of forking
     Isaac-only execution logic

Runtime topology guidance:

- ManyForge core must remain ROS-free at the package boundary.
- ROS dependencies belong in `manyforge_ros` and ROS-capable images.
- The generic ROS image is the qualification lane for adapter behavior.
- Isaac ROS is a deployment specialization, not the only place where ROS
  integration is allowed to work.
- Split-container and single-container deployments should both remain possible:
  - split: `manyforge-core` + ROS adapter sidecar
  - single generic ROS-capable: `manyforge-ros-jazzy` with the required
    ManyForge packages installed or mounted
  - Isaac deployment: `manyforge-isaac-ros`

Near-real-time implication:

- The ROS adapter is part of the execution-critical path, not a casual web
  integration.
- Stop/pause/reset control paths must have dedicated callback groups/executors,
  avoid blocking scene/planning/perception work, and emit measurable latency
  tracepoints.
- The Composer UI path must never be required for correct stop/collision
  semantics.
- The hard real-time servo/safety controller remains outside ManyForge, but
  ManyForge still owns timely, deterministic command issuance and execution
  state semantics.

Initial repo scaffold:

- add a separate `Dockerfile.ros-jazzy`
- add a `ros` docker compose profile using host networking for development DDS
  discovery
- add `manyforge_ros` as the home for ROS adapters and deterministic fake ROS
  targets
- add a first fake `FollowJointTrajectory` action server test target
- keep default `test-all.sh all` unchanged; ROS tests are opt-in through the
  ROS image/lane

Validation note:

- The first fake `FollowJointTrajectory` test target was run successfully inside
  the active disposable Isaac ROS container by copying the `manyforge_ros`
  package into `/tmp` and sourcing `/opt/ros/jazzy/setup.bash`.
- The generic `manyforge-ros-jazzy` image was then refactored to start from
  `ros:jazzy-ros-base-noble`, built successfully, and ran
  `./scripts/test-ros.sh` successfully.
- With the Isaac ROS container stopped and only Isaac Sim running, the generic
  `manyforge-ros-jazzy` image successfully discovered and communicated with the
  live Isaac Sim scene over ROS 2 host networking:
  - discovered `/isaac_sim_control`
  - discovered camera, TF, clock, `/isaac_joint_states`,
    `/isaac_joint_commands`, and `/isaac_gripper_state` topics
  - discovered the root-level `simulation_interfaces` service set
  - called `/get_simulator_features`
  - called `/get_current_world`, returning `isaac_manipulator_scene_light`
  - called `/get_entities`, returning the simulator scene graph
  - called `/get_entity_state` for `/World/soup_can`,
    `/mac_n_cheese_centered`, `/background/table_low_327`, and
    `/World/ur10e_robotiq2f_140_ROS`
  - observed `/isaac_joint_states` at roughly 9-13 Hz in the sample window
  - confirmed `/isaac_joint_commands` and `/isaac_gripper_state` each have an
    Isaac Sim subscriber and no publisher
- This means the Isaac ROS image is not required for the current generic ROS
  adapter and Isaac Sim protocol tests. It remains relevant for NVIDIA Isaac
  ROS perception stacks and Isaac-specific deployment overlays.

Read-only scene mirror probe result:

- Added `manyforge_ros.scene_mirror_probe` as a non-mutating diagnostic client
  for ROS 2 `simulation_interfaces`.
- The probe discovers service names by interface type, supports both enumerated
  scene capture and explicitly configured entity paths, and returns a structured
  snapshot with simulator features, entity info, live pose, optional bounds,
  and per-entity errors.
- Added a fake `simulation_interfaces` contract test covering simulator
  features, entity enumeration, entity info, entity state, bounds, and explicit
  configured-entity capture.
- Validation command:
  `docker compose --profile ros run --rm ros-jazzy ./scripts/test-all.sh ros`
- Live Isaac Sim observation from `manyforge-ros-jazzy`:
  - `/get_simulator_features` responds and advertises entity info/state support
  - `/get_entities` responds, but the returned graph is noisy and includes
    background/visual hierarchy paths such as `/background/Floor`
  - `/get_entity_info` currently reports generic object categories and no tags
    for sampled entities
  - `/get_entity_state` returns live poses for sampled entities
  - `/get_entity_bounds` is not advertised and `ENTITY_BOUNDS` is not listed in
    simulator features
- Implication: the first scene mirror cannot rely on simulator-provided bounds,
  categories, or tags for safe task binding. The next step must define explicit
  configured bindings and conservative geometry derivation, with an Isaac-side
  geometry exporter left open if ROS `simulation_interfaces` remains too weak
  for bounds/collider extraction.

Isaac collider probe result:

- Added `scripts/isaac-collider-probe.py` as a read-only Isaac Sim/Kit Python
  diagnostic script.
- Refactored the extraction logic into `scripts/isaac_collider_probe_lib.py` so
  the CLI, a future Isaac Sim extension, and a future service wrapper can reuse
  the same probe implementation.
- The script inventories prims with `UsdPhysics.CollisionAPI`, authored USD
  collision metadata, mesh approximation mode, PhysX collision schemas,
  transforms, local/world AABBs, primitive geometry fields, and optional PhysX
  cooked convex collision representation summaries.
- The probe now records visibility, computed visibility, purpose, optional
  non-collider prim transforms via `--probe-prim`, and supports
  `--include-hidden` / `--purpose` filters.
- The probe supports two output profiles:
  - `raw_inventory`: engineering/debug output close to the original probe JSON.
  - `llm_scene_authoring`: compact grouped output with source roots,
    collider summaries, ManyForge `box` recommendations, confidence, warnings,
    and traceable source paths.
- Expected usage on the Isaac Sim machine:
  `./python.sh /path/to/manyforge/scripts/isaac-collider-probe.py --output /tmp/isaac_colliders.json --pretty`
- Expected assistant-profile usage:
  `./python.sh /path/to/manyforge/scripts/isaac-collider-probe.py --exclude-subtree /World/ur10e_robotiq2f_140_ROS --probe-prim /World/ur10e_robotiq2f_140_ROS --output-profile llm_scene_authoring --output /tmp/isaac_scene_authoring.json --pretty`
- The script can filter scene scope with repeated `--include-subtree` and
  `--exclude-subtree` arguments, which is the same direction intended for the
  later exporter to exclude robot/URDF-owned content.
- Probe output has confirmed that Isaac Python can provide useful collider,
  transform, visibility, and AABB data that `simulation_interfaces` does not
  expose. The next plumbing step is therefore an Isaac Sim extension service so
  Thor-side ManyForge tools can request the report without manual file copies.

Isaac collider service / assistant workflow direction:

- The Isaac-side collider probe is useful, but the final user workflow should
  not require manually copying JSON files between machines.
- A practical next architecture is an Isaac Sim extension that wraps the same
  collider extraction logic and exposes an on-demand ROS 2 service.
- A native Isaac extension scaffold now exists under
  `exts/manyforge.isaac.collider_service`.
- To avoid early custom ROS interface churn, the first implementation exposes:
  - `/manyforge/isaac_collider_report` as `std_srvs/srv/Trigger`, returning
    the configured default query report in the service response message
  - `/manyforge/isaac_collider_query/request` and
    `/manyforge/isaac_collider_query/response` as `std_msgs/msg/String`
    request/response topics for parameterized versioned JSON queries
- This split is deliberate because ROS 2 core packages do not provide a
  standard string-in/string-out service type. A later production interface may
  replace the topic RPC with a ManyForge-specific service definition if the
  contract stabilizes.
- The string payload must be versioned JSON, not free text.
- The LLM or Nemoclaw assistant should not call this service directly. It should
  call a ManyForge/Nemoclaw tool such as `inspect_isaac_scene`, and that tool
  should validate/build the JSON request, call ROS, validate the JSON response,
  and return an LLM-oriented scene summary.
- The assistant should not need to know whether the data came from ROS, Isaac
  Python, a local file, or a future simulator adapter.
- USD/PhysX stage access must stay on the Isaac/Kit update thread. ROS callbacks
  should enqueue requests and wait for the main-thread capture result rather
  than touching the stage directly from ROS executor threads.

Request shape for the first service spike:

```json
{
  "version": "manyforge.isaac.collider_query.v0.1",
  "include_subtrees": ["/World"],
  "exclude_subtrees": ["/World/ur10e_robotiq2f_140_ROS"],
  "include_hidden": false,
  "include_cooked_convex": false,
  "max_colliders": 200,
  "output_profile": "llm_scene_authoring"
}
```

Required response profiles:

- `raw_inventory`: close to the current probe output, for debugging and
  engineering validation.
- `llm_scene_authoring`: grouped by likely logical object, with source paths,
  collider summaries, recommended ManyForge `box`/`sphere` approximations,
  uncertainty, warnings, and reasons. This is the profile intended for assistant
  scene-construction workflows.

The LLM-oriented response should preserve the full acquired scene information
unless the user or tool explicitly scopes the query. The goal is not to hide
crowded-scene data from the assistant. The goal is to present the complete
inventory in a structure that the assistant can reason over with tools and user
selection.

- group collider fragments under stable source roots when possible
- retain the full grouped object list even for crowded scenes
- retain exact source paths for traceability
- propose ManyForge-compatible shapes only when the source data supports a
  defensible approximation
- explicitly mark skipped, unsupported, hidden, or high-uncertainty objects
- support tool-side search, ranking, grouping, and user-selected inclusion
  rather than destructive pre-filtering
- allow explicit robot/URDF subtree exclusion when generating a ManyForge scene,
  because robot collision geometry belongs to the robot model, not scene
  resources

This service can later grow a direct ManyForge YAML export mode, but the first
assistant-facing goal is a structured scene-understanding report. The
programmer still reviews and validates any generated ManyForge scene.

Immediate service validation steps:

1. Install the extension on the Isaac Sim laptop by adding the repository
   `exts` directory as an Isaac extension search path.
2. Enable `manyforge.isaac.collider_service` with the loaded scene running.
3. From the ROS-capable ManyForge container on Thor, call
   `/manyforge/isaac_collider_report` and verify a JSON response arrives over
   the LAN.
4. Send a parameterized query through
   `/manyforge/isaac_collider_query/request` and verify the correlated response
   on `/manyforge/isaac_collider_query/response`.
5. Compare the service response with the previous script-generated
   `isaac_scene_authoring.json` output to confirm parity before building
   assistant tools on top.

Live validation result:

- The extension was installed on the Isaac Sim laptop as a local user extension
  using the copied, self-contained `manyforge.isaac.collider_service` folder.
- The ROS-capable `manyforge-ros-jazzy` container on Thor discovered:
  - `/manyforge/isaac_collider_report` as `std_srvs/srv/Trigger`
  - `/manyforge/isaac_collider_query/request` as `std_msgs/msg/String`
  - `/manyforge/isaac_collider_query/response` as `std_msgs/msg/String`
- The Trigger service returned a successful `llm_scene_authoring` report with:
  - `collider_count=63`
  - `object_count=62`
  - `error_count=0`
  - robot prim transform for `/World/ur10e_robotiq2f_140_ROS`
- A scoped String topic request for `/World/soup_can` returned a correlated
  response with `request_id=thor-test-2`, `collider_count=2`,
  `object_count=1`, and `error_count=0`.
- The first shell-only topic publish attempt failed because nested shell/YAML
  quoting produced invalid JSON. Assistant/tool clients should use a real ROS
  client library or otherwise validate the exact JSON string before publishing.

ManyForge-side `inspect_isaac_scene` tool:

- Added `manyforge_ros.inspect_isaac_scene` as the deterministic ManyForge-side
  client for the Isaac collider service.
- The tool uses the parameterized String request/response topics, not the
  Trigger default service, so every call can be scoped and correlated by
  `request_id`.
- It validates:
  - request/response JSON version
  - matching `request_id`
  - boolean `success`
  - failure payloads
  - required summary integer fields
  - object list shape when present
- It exposes both a Python API for future Nemoclaw/assistant tools and a CLI
  for current manual and integration testing:
  `python3 -m manyforge_ros.inspect_isaac_scene --include-subtree /World/soup_can --timeout 30`
- Live validation from the ROS-capable ManyForge container against the active
  Isaac Sim extension returned one `/World/soup_can` grouped object with two
  colliders and no errors.
- The next assistant-facing step is not ROS plumbing. It is defining how the
  assistant/user selects scene roots and how selected objects become reviewed
  ManyForge scene-resource edits.

Deferred direct Composer import UI:

- A direct Scene-tab entry point such as `Import from Simulator...` remains
  valuable, but it is postponed until the assisted workflow is proven or
  rejected.
- The direct UI should not clutter the main Scene tab with permanent simulator
  discovery state. It should be a requested modal or side panel that supports
  search/filter/grouping, lets the user select one or a few logical simulator
  objects, and then converts only those selections into reviewed scene drafts.
- The authored ManyForge scene should remain the persistent Composer surface.
  Simulator candidates are temporary inspection results until the user accepts
  them as scene draft edits.
- This deferred UI path should reuse the same deterministic tools as the
  assistant path rather than becoming a separate importer.

Assistant implementation direction:

- Start the assistant workflow before implementing the direct simulator-import
  UI because the assisted scene-understanding route is strategically higher
  impact and may change the base review/apply workflow.
- The assistant is an assisted route, not an autonomous route. It should answer
  user questions about the inspected scene, propose candidate roots and
  conservative planning geometry, and explain uncertainty, while the user
  remains responsible for review using both ManyForge and Isaac Sim.
- The assistant must not write YAML or mutate Composer state directly. It must
  call constrained tools that inspect, propose, validate, add/update/remove
  scene draft objects, and add valid object-operation nodes.
- If assistant integration proves not viable in the short term, the same tools
  still support the postponed direct Composer import UI.

Omniverse Libraries relevance:

- NVIDIA announced a modular Omniverse Libraries direction in April 2026:
  `ovrtx`, `ovphysx`, and `ovstorage` expose rendering, PhysX simulation, and
  storage as standalone, headless-first APIs with C++ and Python bindings.
- The article also describes MCP-enabled Omniverse tooling for agentic
  orchestration, which is directionally aligned with hiding simulator details
  behind explicit tools rather than letting the LLM call low-level APIs
  directly.
- This is relevant to ManyForge because a future simulation adapter may not need
  to depend on a full Isaac Sim/Kit application process. A later route could
  use standalone Omniverse/PhysX libraries for headless collider and simulation
  services.
- Near-term caveat: these libraries are early access and API-stability/LTS
  claims are not yet production-ready. The current near-term path remains the
  Isaac Sim/Kit Python probe, then an Isaac Sim extension service if needed.

Following next steps after service plumbing:

1. Define the assistant tool contract for simulator-backed scene authoring.
   Start with read-only inspection and non-mutating proposal tools, then add
   constrained mutation tools that operate on Composer/program drafts.
2. Define explicit scene binding rules before relying on entity names or
   collider roots.
   The live entity graph is noisy and includes visual, collision, prototype,
   action-graph, and background nodes. The adapter must use configured filters
   or semantic bindings rather than broad regex guesses.
3. Define the first conservative planning-geometry derivation path.
   Start with configured dimensions or manually approved primitive
   approximations. Do not assume bounds/colliders are available through
   `simulation_interfaces`.
4. Keep direct Composer `Import from Simulator...` as a deferred UI path.
   It should reuse assistant/tool contracts after the assisted workflow has
   clarified the review/apply model.
5. Build a direct Isaac command probe only as a controlled execution spike.
   It should read the current joint state, validate expected joint names, apply
   a very small bounded delta, and publish to `/isaac_joint_commands`.
6. Build or choose the `FollowJointTrajectory` bridge path based on the direct
   command result.
   Long-term ManyForge execution should target standard trajectory semantics,
   cancellation, feedback, tolerances, and traceability rather than raw topic
   fire-and-forget commands.
7. Keep Isaac ROS as a downstream specialization.
   Generic ROS adapter tests should continue to pass in `manyforge-ros-jazzy`;
   Isaac ROS should be used when tests require NVIDIA perception packages,
   Isaac Manipulator reference workflows, or Thor deployment parity.

### Mesh Object Representation Constraint

RoboPlan cannot currently load arbitrary scene meshes as collision objects in
the ManyForge scene. The simulator scene mirror must therefore separate:

- visual/source metadata: simulator entity id, mesh or USD source path when
  available, live pose, semantic binding, and trace provenance
- operational planning geometry: conservative primitives accepted by RoboPlan
  and ManyForge scene resources

Baseline policy for the first preview:

- represent mesh objects in RoboPlan with conservative primitives, usually an
  oriented bounding box when a reliable object pose is available
- use axis-aligned boxes only when orientation is unknown or cannot be trusted
- represent support surfaces such as tables as boxes or planes where supported
- keep the original simulator entity id and source metadata attached to the
  mirrored object so the approximation is auditable
- require configured bindings or validated entity filters for task-relevant
  objects; broad scene-graph guesses are not acceptable for execution
- fail closed when a relevant object has no validated conservative collision
  approximation

Open decisions:

- whether to add mesh collision support to RoboPlan/ManyForge later
- whether to add convex hull or decomposed primitive support before full mesh
  support
- how to expose approximation quality/confidence in Composer and traces
- how to keep visual mesh display in Viser aligned with simplified planning
  geometry without implying that the planner used the exact mesh

### Logical Entity vs Planning Geometry

Future simulator and perception adapters should treat a scene object as one
logical entity that may be represented by multiple planning geometries.

This is different from exposing every collider fragment as an independent
scene object. The public scene model should stay semantic:

- a simulator object, part, bin, fixture, or workpiece has one stable logical id
- the logical entity carries source metadata such as simulator path, mesh or USD
  resource, live pose, semantic class, and observation provenance
- the logical entity owns one or more planning geometry fragments used by
  RoboPlan, for example boxes, spheres, cylinders, convex hulls, or convex
  decomposition parts
- internal RoboPlan object names may be expanded as implementation details,
  for example `part_A::__geom_0`, but they must not become user-facing object
  identities

This keeps authoring, LLM tools, traces, attach/detach, delete, reset, and
visibility controls aligned with the physical or simulated object the user
understands. It also avoids a misleading scene tree where one real object looks
like many unrelated objects.

Recommended contract:

- define public scene operations on the logical entity id
- apply geometry updates atomically by replacing the entity's full internal
  planning geometry set
- preserve the source mesh and simulator entity metadata even when planning uses
  simplified geometry
- expose approximation type, source, timestamp, and confidence in traces and
  diagnostics
- fail closed if a task-relevant entity has no validated planning geometry
- keep visual geometry and planning geometry explicitly separate in Composer and
  Viser

This remains useful even if RoboPlan later adds true mesh collision support.
Industrial workflows often need collision geometry that differs from visual
geometry for safety, performance, determinism, or process reasons. A semantic
entity with one or more explicit planning geometries supports both future mesh
colliders and current conservative primitive approximations.

Open questions for the RoboPlan maintainers:

- whether arbitrary fixed-world mesh collision objects are planned on their
  roadmap
- whether convex hull or compound primitive collision objects are supported or
  planned before full mesh support
- whether RoboPlan can accept a compound collision object under one logical
  scene id, or whether ManyForge should keep expanding compound objects into
  flat internal names
- what collision backends, mesh formats, scale/origin policies, and performance
  limits would be acceptable for deterministic industrial use

## Decision Structure

The implementation choice should not be treated as one monolithic decision.

It should be split into two related but distinct adapter problems:

### A. Simulator world / entity path

This includes:

- initial live scene capture
- runtime world state query
- entity list/query
- frame inventory
- optional world/entity mutation

### B. Robot / gripper control path

This includes:

- robot discovery
- trajectory execution
- stop/reset
- gripper command/status

These two slices may land on different implementation routes.

That is acceptable as long as both still fit the simulator adapter contract and
do not create a parallel runtime architecture.

## Why `simulation_interfaces` Matters

`simulation_interfaces` is relevant because it is becoming a real
cross-simulator standard for simulator/world/entity interaction.

It appears to have current support in:

- Gazebo
- O3DE
- Isaac Sim

That makes it a strong candidate for a future ManyForge simulator adapter
contract.

But it is still a ROS-based path, so using it immediately would also mean
taking on ROS-wrapper work now. That may or may not be the right short-term
choice.

## Minimum Preview Capability Slice

The first serious simulator-backed preview should solve only the minimum slice
needed to show a useful ManyForge simulation-operator workflow.

### Required now: simulator world / entity slice

- live world query
- entity list/query
- frame inventory
- initial live scene capture
- ongoing live scene updates

### Required now: robot / gripper slice

- robot discovery
- gripper discovery
- robot trajectory execution
- robot stop/reset
- gripper command/status

### Explicitly out of scope for the first slice

- USD/stage parsing
- asset-level import richness
- broad scene-edit coverage beyond what the scenario actually needs
- perception productionization
- broad simulator portability proof
- assistant autonomy beyond reviewable proposal/apply

The preview should prove one useful path, not prematurely generalize the whole
simulation story.

## Decision Matrix

The candidate routes should be compared against the simulator contract and the
minimum preview slice, not only on general familiarity.

### Evaluation Criteria

- fit to the simulator adapter contract
- implementation scope now
- reuse later
- portability to other simulators
- risk of throwaway work
- sim/real architectural symmetry
- observability and traceability fit

### Route Comparison

| Route | Simulator world / entity path | Robot / gripper path | Strengths | Main risks |
|---|---|---|---|---|
| Direct Isaac | strong fit for a narrow first implementation | possible, but Isaac-specific unless wrapped carefully | lowest short-term scope, direct access to live simulator state | simulator-specific drift, weak portability if boundary discipline slips |
| ROS 2 `simulation_interfaces` centered | strong candidate for world/entity operations | incomplete by itself for full robot/gripper control | better cross-simulator story, less throwaway work on the environment side | pulls ROS-wrapper work in early, does not by itself solve robot actuation |
| Other ROS wrappers centered | weaker as the main world/entity abstraction | stronger candidate for robot/gripper control continuity | closer to real-robot ROS control patterns | can broaden scope too early, may still be simulator-specific |
| Hybrid | can use the strongest path per slice | can use the strongest path per slice | most realistic if slices genuinely differ | easiest to overcomplicate if boundaries are not explicit |

Current outcome:

- the world/entity path should start with ROS 2 `simulation_interfaces`
- the durable robot execution path should target ROS 2 control /
  `FollowJointTrajectory`
- direct Isaac topics remain a diagnostic fallback only
- MoveIt/cuMotion remains a reference or optional interoperability path
- direct Isaac Python/extension work is limited to read-only collider
  introspection; it is not the robot execution route

## Next Spike Recommendation

Before putting the assistant on the critical path, turn the discovered route
into a deterministic vertical slice:

1. connect to the live simulator
2. capture the initial live scene
3. reconstruct the relevant ManyForge operational scene
4. preserve mesh/source metadata while using conservative planning primitives
5. keep runtime scene updates flowing
6. execute one robot motion path through `FollowJointTrajectory`
7. execute one gripper command path with explicit feedback/status semantics

If this vertical slice is weak, assistant work on top of it is premature.

## Practical Work Breakdown

### Phase 0: Decide the minimum preview workflow

Lock the concrete use case:

- pick and place
- bin picking

Lock what “done” means:

- simulator scene can be queried
- ManyForge scene/resources can be reconstructed
- one robot/gripper control path can be proven end to end
- assistant can then propose a usable program on top of that foundation

### Phase 1: Define the simulator-facing capability slice

Define the minimum needed for the first preview:

- live world query
- entity list/query
- frame inventory
- initial live scene capture
- ongoing live scene updates
- robot discovery
- gripper discovery
- robot trajectory execution
- robot stop/reset
- gripper command/status

### Phase 2: Initial live scene capture path

Build deterministic tools that:

- capture the initial live simulator scene
- extract the subset relevant to ManyForge authoring
- reconstruct that subset in the ManyForge backend/HMI scene

Constraint:

- for the baseline route, initial bootstrap and ongoing runtime scene updates
  should come from the same live simulator interface
- USD/stage parsing is not the baseline path for this preview

### Phase 3: Assistant-backed authoring path

Use the deterministic scene tools to support:

- scene summarization
- proposal of ManyForge scene artifacts
- proposal of ManyForge program/tree artifacts
- review/apply in Composer

### Phase 4: Presentable scenario

Deliver one repeatable scenario with:

- gripper-aware manipulation
- clear runtime state
- visible scene alignment
- retained evidence of success/failure

## Relationship To The Broader Simulation Story

Isaac Sim is the first tactical simulator target.

It should also serve as research input for a broader future ManyForge
simulation path, which may later deserve explicit treatment in
`manyforge_specs` as a simulator adapter family.

That broader step is not required to execute the current preview plan, but this
plan must avoid blocking it.
