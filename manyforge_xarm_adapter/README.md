# ManyForge xArm adapter

Experimental, dedicated ROS 2 boundary for the Lite6 real-robot work. It keeps
xArm model generation, endpoint resolution, vendor-state observation, and
attended command-evidence capture out of the ROS-free kernel and out of
generic deployment code.

The connected XC-6 slice remains deliberately observation-only:

- the fixed pneumatic tool is generated from the xArm builder as geometry,
  with no invented tool joint or actuation;
- the robot address is accepted only at launch from
  `MANYFORGE_LITE6_ROBOT_IP` and is not stored in model or binding manifests;
- the connected endpoint must report six axes and the Lite6 SDK device type;
  the adapter does not query, configure, compare, or retain a unit serial;
- startup does not create a controller manager, trajectory controller, tool
  provider, vendor command subscription/service, or action server (ordinary
  ROS parameter/introspection services remain); and
- the patched xArm standalone driver suppresses automatic fault clearing and
  shutdown mode changes when `read_only=true`, and closes its observation
  threads before ROS invalidates the node context.

The raw `/ufactory/joint_states` stream is not projected directly into the
robot model. Some Lite6 firmware reports a zero-filled placeholder while the
drives are disabled and has not yet established the physical joint position.
The observer therefore withholds model projection until one error-free sample
has all expected brake/servo bits enabled and the rich report angles agree
with the polled joint stream within 0.002 rad. It then latches initialization
and republishes only structurally valid samples on
`/manyforge/xarm/validated_joint_states`. A later stop may freeze the angles in
the rich status report while the joint stream records mechanical settling;
that divergence is diagnosed explicitly while the validated joint stream
continues to represent the observed pose. None of these observations opens
motion admission.

The production supervised URDF separates fast motion fencing from terminal
session-loss classification. A provisional 1000 ms producer observation lease bounds both
robot-state validity and the internal xArm write gate; generic admission
publication retains its independent 250 ms lease. If rich reports or
successful control reads remain absent for 2000 ms, the xarm_ros2 owner marks
that SDK session terminal and requires replacement of the controller-manager
process. Reconnecting the cable never reuses or re-arms the old session.

This package and its image are local experimental inputs, not a supported
real-robot lane. The production binding now selects the supervised
`xarm_ros2` hardware plugin as the sole SDK owner. ManyForge reaches its
restricted state/request/result surface only through ROS; the separate
ManyForge hardware plugin is fake/HIL-only. Lifecycle primitives, arming, and
all motion remain later physical gates. See
[`../docs/operations/LITE6_LIVE_QUALIFICATION.md`](../docs/operations/LITE6_LIVE_QUALIFICATION.md)
for the staged acceptance boundary and evidence rules.

## Offline workflow

Build the image from the exact local XC-6 xArm worktree:

```bash
MANYFORGE_XARM_SOURCE_CONTEXT=/path/to/xarm_ros2 \
  ./scripts/xarm/build-real-lite6-image.sh
```

The image build compiles the fork and must pass the focused xArm access-policy,
driver-lifecycle, supervised-session, and restricted ROS command-contract C++
tests. It intentionally does not use the fork's package-wide legacy lint suite
as an XC-6 gate.

Generate the fixed-tool model, immutable endpoint/controller binding, and
independent validation result without network access:

```bash
docker run --rm --network none \
  -v "$PWD:/workspace" -w /workspace \
  manyforge-ros-jazzy-real-lite6:experimental \
  ./scripts/ros/generate-lite6-fixed-pneumatic-model.sh
```

The generated binding freezes the controller-manager identity, JTC
name/type, action endpoint, joint order, position/velocity command and state
interfaces, update/publication rates, activity QoS, runtime freshness policy,
supervised state/request/gate endpoints, JTC cancel-deceleration contract, and
artifact digests. The focused fake lane loads the fake-owned equivalent
through the real Jazzy controller manager; the production artifact names the
xarm owner plugin.

The resolved runtime manifest also projects those already-validated facts into
`ros2_control_execution_binding` using the robot-neutral schema
`manyforge.ros2_control.execution_binding.v0.experimental`. This projection
adds no inferred prefix, namespace, endpoint, or vendor state: the generic
execution backend consumes exact resolved names and verifies them against
controller-manager activity and services. The xArm-specific binding remains
the source of generation identity and artifact provenance.

The resolved runtime manifest now also carries:

- `execution_admission`, which freezes the topic, per-process provider
  identity, supervisor identity, refresh period, and maximum lease; and
- `manyforge_execution_config`, which projects the exact action/state/joint
  endpoints, controller binding, observed-state policy, and mandatory
  admission transport into one ready-to-inject generic execution config; and
- `cell_supervision`, which freezes the cell/component dependency graph,
  receiver-local stale budgets, restart domains, and the external
  supervisor's per-start epoch and heartbeat path.

The supervised-owner qualification launcher starts the manifest-bound
admission provider beside `controller_manager` and robot-state publication,
validates the loaded JTC's read-only cancel-deceleration parameters before
publishing owner readiness, and treats an unexpected exit of any managed
process as a topology failure. A generated/runtime/live mismatch therefore
keeps execution admission closed. This remains an isolated qualification
surface. The product-shaped `real-robot` launcher consumes
`manyforge_execution_config` and the retained runtime record from a separate,
non-owner Composer service. A host-side cell supervisor observes both
containers and owns bounded dependency-ordered teardown; its arm/shutdown
channel is not writable from either container. Startup remains fail-closed
until the selected headless authority establishes a lifecycle session.
Credential authentication and the PLC/ROXSIE authority adapter remain
separate release work.

Resolve the runtime-only values without network access. The resolver verifies
the source manifest, template and controller digests, exact token count,
portable identities, authority profile, model equivalence, and every
controller-binding field before writing a new runtime manifest. Only the
robot address and xarm transport-owner ID are substituted into the URDF;
robot-instance and authority identity remain supervisory manifest data:

```bash
docker run --rm --network none \
  -e MANYFORGE_LITE6_ROBOT_IP=192.168.1.31 \
  -e MANYFORGE_LITE6_RUNTIME_BINDING_CONFIRM=RESOLVE_SUPERVISED_RUNTIME \
  -e MANYFORGE_LITE6_LIFECYCLE_PROVIDER_INSTANCE_ID=lite6-a:provider:offline-example \
  -e MANYFORGE_CELL_SUPERVISOR_COMPONENT_EPOCH=offline-supervisor-example \
  -e MANYFORGE_CELL_SUPERVISOR_HEARTBEAT_PATH=/tmp/manyforge-real-lite6/cell-supervisor-heartbeat.json \
  -v "$PWD:/workspace" -w /workspace \
  manyforge-ros-jazzy-real-lite6:experimental \
  ./scripts/ros/resolve-lite6-supervised-runtime.sh
```

The resolved artifact retains the deployment address by design and therefore
lives only under ignored runtime state. It is not a source/model artifact.

Run the focused package, launch-graph, and lint suite:

```bash
docker run --rm --network none \
  -v "$PWD:/workspace" -w /workspace \
  manyforge-ros-jazzy-real-lite6:experimental \
  ./scripts/ros/test-real-lite6-runtime.sh
```

The default launch remains disconnected:

```bash
docker run --rm --network none \
  -v "$PWD:/workspace" -w /workspace \
  manyforge-ros-jazzy-real-lite6:experimental \
  ./scripts/ros/launch-lite6-read-only.sh
```

Do not enable physical connection until the plan's physical checkpoint is
scheduled with the independent stop path available. Connection requires both
`MANYFORGE_LITE6_CONNECT_READ_ONLY=ON` and the literal confirmation
`MANYFORGE_LITE6_READ_ONLY_CONFIRM=CONNECT_READ_ONLY`. Address ownership is a
deployment-network prerequisite: an IP collision must be resolved before
launch rather than disambiguated in the robot adapter.

## Physical read-only checkpoint

This checkpoint observes one stationary robot. It does not start
`controller_manager`, a trajectory controller, or a tool provider, and it
does not authorize motion.

Before connecting:

- keep the cell clear and the independent stop/E-stop path immediately
  available;
- verify the robot is stationary and record its current mode, state, warnings,
  and errors at the pendant;
- stop every other xArm driver, ManyMove process, controller manager, and
  simulator bridge that could own this robot; and
- verify that the experimental image was rebuilt from the intended clean xArm
  commit and passed `test-real-lite6-runtime.sh`.

From the ManyForge repository, read the address into the environment and start
the observation-only container:

```bash
read -r -p "Lite6 IPv4 address: " MANYFORGE_LITE6_ROBOT_IP
export MANYFORGE_LITE6_ROBOT_IP

docker run --rm --init --interactive --tty \
  --name manyforge-lite6-read-only \
  --network host \
  -e MANYFORGE_LITE6_ROBOT_IP \
  -e MANYFORGE_LITE6_CONNECT_READ_ONLY=ON \
  -e MANYFORGE_LITE6_READ_ONLY_CONFIRM=CONNECT_READ_ONLY \
  -e MANYFORGE_REAL_LITE6_OUTPUT_ROOT=/tmp/manyforge-real-lite6 \
  -v "$PWD:/workspace:ro" -w /workspace \
  manyforge-ros-jazzy-real-lite6:experimental \
  ./scripts/ros/launch-lite6-read-only.sh
```

If the cell uses a non-default `ROS_DOMAIN_ID`, pass that existing environment
value with an additional `-e ROS_DOMAIN_ID`.

While the container remains attached, inspect it from a second terminal:

```bash
docker exec manyforge-lite6-read-only bash -lc '
  set +u
  source /opt/ros/jazzy/setup.bash
  source /opt/manyforge/xarm/install/setup.bash
  source /tmp/manyforge-build/real-lite6/install/setup.bash
  set -u
  ros2 node list --no-daemon
  ros2 action list
  ros2 service list --no-daemon
  ros2 topic echo --once /manyforge/xarm/diagnostics
  ros2 topic echo --once /ufactory/joint_states
  ros2 topic echo --once /manyforge/xarm/validated_joint_states
'
```

Or capture the same gate automatically, with bounded raw output, return codes,
timeouts, postconditions, digests, and host-side provenance:

```bash
./scripts/xarm/capture-lite6-live-evidence.sh
```

The output is retained under the ignored
`.manyforge-var/real-lite6/live-evidence/` runtime tree. A timeout never
counts as a pass. The validated joint-state observation alone is optional
because a drive-disabled robot may not yet expose a trustworthy physical
pose.

The expected graph contains only `/ufactory_driver`,
`/lite6_robot_state_publisher`, and
`/manyforge_xarm_read_only_observer`. The action list must be empty. Services
must be limited to ordinary ROS parameter/introspection services for those
three nodes: no xArm motion, enable, mode, fault-clear, gripper, or trajectory
endpoint may appear. Diagnostics must continue to report
`access_mode=read_only`, `motion_admission=closed`, and
`command_endpoints=suppressed`. Before drive-side position initialization,
diagnostics must also report `joint_position_initialized=false` and the
validated joint topic must remain silent. After initialization they report
the position validity and whether the two vendor sources agree. Record any
observed vendor fault; do not clear it as part of this checkpoint.

The driver must log that the connected endpoint reports six axes and the Lite6
SDK device type before startup inspection. An axis/type mismatch closes the
connection before fault inspection or command initialization. ICMP/ping and
neighbor/MAC observations are network preflight evidence only; a conflicting
address is repaired in network configuration, not selected around at runtime.

Confirm at the pendant that connection and shutdown caused no movement, drive
enable, mode change, warning/error clear, or tool command. Stop with `Ctrl-C`
and remove the runtime address from the shell:

```bash
unset MANYFORGE_LITE6_ROBOT_IP
```

## Supervised ROS observation preflight

This checkpoint attaches to the restricted ROS state surface of an
already-running supervised xarm controller-manager process. It never creates
a second SDK session and issues no lifecycle or trajectory command:

```bash
docker run --rm --init --network host \
  -e MANYFORGE_LITE6_TRANSPORT_OWNER_ID=lite6-a:sole-owner \
  -v "$PWD:/workspace:ro" -w /workspace \
  manyforge-ros-jazzy-real-lite6:experimental \
  ./scripts/ros/run-lite6-supervised-preflight.sh
```

The JSON result must prove a fresh exact six-axis/device-9 identity and that
the observed lifecycle-command counter did not change during the window.
Robot state depends on the attended test stage; observation alone does not
qualify a lifecycle primitive.

## Attended supervised command evidence

The qualification-only supervised owner launcher starts the exact xarm_ros2
owner with the hardware component inactive. It requires clean ManyForge
source, the experimental real-Lite6 image label, one literal confirmation,
host networking, host IPC, and no already-labeled owner:

```bash
export MANYFORGE_LITE6_ROBOT_IP=192.168.1.31
export MANYFORGE_LITE6_SUPERVISED_CONFIRM=START_SUPERVISED_OWNER_INACTIVE
./scripts/xarm/launch-lite6-supervised-owner.sh
```

Startup builds the four ManyForge ROS packages in the container, generates
and resolves the ignored runtime binding with a fresh provider-session UUID,
then starts robot-state publication, controller manager with `lite6_system`
inactive, and the observation-only admission provider. It does not establish
authority, issue a lifecycle primitive, activate hardware/JTC, or send a
trajectory. Keep this terminal attached and stop it with `Ctrl-C`.

Only during an explicitly scheduled attended Stage C row, use a second
terminal to capture one reviewed action:

```bash
export MANYFORGE_LITE6_ACTION=robot.stop_control
export MANYFORGE_LITE6_ACTION_CONFIRMATION=robot.stop_control
./scripts/xarm/capture-lite6-supervised-command.sh
```

The wrapper refuses an unlabeled container, private network/IPC topology,
missing runtime binding/install, unknown action, or inexact confirmation. The
installed client routes the action through
`LocalOperatorGateway -> XArmLifecycleRosPort -> restricted xarm_ros2 ROS
service`; it never uses the SDK directly. The retained ignored evidence binds
the action/request/session/principal, before/after observations, catalog
postcondition, exact primitive and owner return, gateway audit, timeouts, raw
output digest, image, and source tree. It reports success only after the
observed postcondition and authority closure both pass.

This is not a sequence runner. Run one command at a time, inspect its evidence,
and obtain the next physical approval separately. It is also not the future
credential-authenticated Composer/PLC ingress.

Enable and Start Control additionally arm `lifecycle_transient.py` before the
command. It records joint/state initialization and computes deviation only
from owner-qualified poses. If the sampler cannot arm, the physical request is
not sent. A missing explicit bound remains visible as pending adjudication
rather than being replaced by a default tolerance.

The separate no-trajectory controller/gate row is:

```bash
export MANYFORGE_LITE6_CONTROLLER_GATE_CONFIRMATION=\
QUALIFY_NO_TRAJECTORY_CONTROLLER_GATE
export MANYFORGE_LITE6_CONTROLLER_GATE_POSE_LIMIT_RAD=0.001
./scripts/xarm/capture-lite6-controller-gate.sh
```

`controller_gate_evidence.py` switches only the binding-selected JTC, never
publishes a trajectory, and binds controller activity, the 100 ms gate lease,
automatic expiry, write/lifecycle counters, joint samples, and the explicit
pose limit into one ignored evidence artifact. Its offline tests qualify the
capture path only; physical promotion still requires the attended runbook.

## Ownership

- `profile.py` validates builder inputs and contains no cell-local network
  value.
- `model_generation.py` generates and hashes URDF, SRDF, kinematics, joint
  limits, and controller artifacts from the image-pinned builder.
- `binding.py` is a ROS-free, deterministic compiler for namespaced/prefixed
  endpoints.
- `binding_manifest.py` ties the source, generated model, and resolved
  endpoints/controller contract together without retaining the robot address.
- `controller_binding.py` compiles and freezes the exact generated
  controller-manager/JTC contract and projects it into the generic
  ros2_control execution-binding schema.
- `supervised_runtime_binding.py` replaces the two hardware-owned runtime
  tokens, records robot/authority/provider identity separately, recompiles the
  controller binding, and emits a digest-bound runtime-only manifest plus its
  exact generic execution configuration.
- `execution_admission_provider_launch.py` validates that manifest and launches
  the provider with exact ROS parameters; it never derives vendor endpoints.
- `live_evidence.py` captures bounded read-only and supervised-preflight
  observations with explicit postconditions.
- `supervised_command_evidence.py` runs one installed, gateway-routed
  lifecycle command and retains bounded identity/postcondition/audit evidence.
- `lifecycle_transient.py` samples owner-qualified joint/state transitions
  around Enable and Start without creating a command endpoint.
- `controller_gate_evidence.py` captures one explicit, no-trajectory named-JTC
  re-arm and 100 ms current-pose gate transaction.
- `read_only_observer.py` turns vendor state topics into diagnostics while
  keeping motion admission closed.

The xArm fork owns vendor-driver behavior, the production ros2_control data
path, and the sole SDK session. `manyforge_xarm_control` owns the NRT ROS
client, xArm semantic mapping, and fake/HIL plugin; this model and observation
package must not acquire or duplicate the production transport.
