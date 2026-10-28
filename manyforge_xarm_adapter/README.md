# ManyForge xArm adapter

> **Current `dev` boundary:** no Lite6 observation, supervised-runtime,
> execution-admission, command-evidence, or real-robot lane is available.
> Runtime-effectful console and launch entrypoints are stable
> typed-unavailable gates; they reject before input parsing, ROS
> initialization, process launch,
> filesystem mutation, network access, or hardware access. The retained
> surface is offline model/binding work plus effect-free validation,
> evidence-planning, and reduction helpers. None of it qualifies a product
> lane.

This package is the retained xArm authoring and qualification boundary. It
does not currently own an SDK session, measured robot state, lifecycle,
admission, Stop, command dispatch, or evidence capture. The full pre-cutover
implementation remains reimplementation and acceptance input at ManyForge
commit `08222188293df8e12f90dc72e204c21f22f92ea1`; the selected xArm fork is
`xarm_ros2@c3172e57c1c1bb2982fc008304ad6bc886357c87`.

## Current retained surface

- `profile.py`, `binding.py`, `model_generation.py`,
  `controller_binding.py`, `binding_manifest.py`, and
  `controller_binding_validation.py` retain offline profile, naming, model,
  controller-artifact, and integrity work. Their experimental artifacts are
  qualification/reimplementation inputs, not proof of an active runtime.
- `live_controller_parameter_validation.py` retains pure expected-value
  projection and supplied-observation comparison. Live ROS lookup returns at
  I10/R.
- `live_evidence.py` retains command-free probe descriptions, bounded supplied
  result classification, and postcondition reduction. It does not run probes
  or write evidence; those capabilities return at I10/R.
- `controller_gate_evidence.py` retains an effect-free pose-deviation
  reducer. Lifecycle transactions return at I7/L0, Kernel admission and Stop
  evidence at I8/P5, and attended xArm evidence at I10/R.
- R7 package 5 (2026-10-25) removed the supervised mode: the supervised
  endpoint names, the supervised hardware parameters and their refusal, and
  the supervised lifecycle transient sampler. The real lane uses the
  unmodified vendor driver; its xArm adapter is `manyforge_xarm_control`.
- `supervised_runtime_binding.py` retains only literal usable-unicast IPv4
  validation from its old resolver. Retained products and addressing must pass
  I3 and I6/N, lifecycle and Kernel ownership must pass I7/L0 and I8/P5, and
  the focused runtime client returns at I10/R.

The following installed names remain deliberately present as stable failure
boundaries, not dormant implementations:

| Entrypoint | Current disposition | Restoration responsibility |
| --- | --- | --- |
| `manyforge-xarm-read-only-observer` | Duplicate observer removed; typed unavailable | Selected measured-state adapter at I10/R; headless status/alarm projection at I5/H0 |
| `manyforge-xarm-resolve-supervised-runtime` | Old singleton resolver removed; typed unavailable | I3, I6/N, I7/L0, I8/P5, then I10/R |
| `manyforge-xarm-run-execution-admission-provider` | Brand-specific legacy provider removed; typed unavailable | I7/L0, I8/P5, and I10/R |
| `manyforge-xarm-resolve-real-robot-deployment` | Old deployment-v0 resolver removed; typed unavailable | I3, I6/N, I7/L0, I8/P5, then I10/R |
| `manyforge-xarm-validate-live-controller-parameters` | Pure comparison retained; live lookup unavailable | I10/R |
| `manyforge-xarm-capture-live-evidence` | Pure planning/reduction retained; runner unavailable | I10/R |
| `manyforge-xarm-capture-supervised-command` | Legacy authority/session runner removed; typed unavailable | I7/L0, I8/P5, and I10/R |
| `manyforge-xarm-capture-controller-gate` | Renewable-gate runner removed; typed unavailable | I7/L0, I8/P5, and I10/R |
| `lite6_real_read_only.launch.py` | Direct observer launch removed; typed unavailable | I10/R |

## Historical commissioning baseline — unavailable

Everything below this heading records how the last connected XC-6 slice was
designed and qualified. Its commands, environment variables, ROS graph, old
runtime manifests, authority/session model, and renewable command gate are
historical acceptance input only. **Do not execute these recipes on current
`dev` or treat their named endpoints as supported contracts.**

The last connected XC-6 slice was deliberately observation-only:

- the fixed pneumatic tool was generated from the xArm builder as geometry,
  with no invented tool joint or actuation;
- the robot address was accepted only at launch from
  `MANYFORGE_LITE6_ROBOT_IP` and was not stored in model or binding manifests;
- the connected endpoint had to report six axes and the Lite6 SDK device type;
  the adapter did not query, configure, compare, or retain a unit serial;
- startup did not create a controller manager, trajectory controller, tool
  provider, vendor command subscription/service, or action server (ordinary
  ROS parameter/introspection services remained); and
- the patched xArm standalone driver suppressed automatic fault clearing and
  shutdown mode changes when `read_only=true`, and closed its observation
  threads before ROS invalidates the node context.

The raw `/ufactory/joint_states` stream was not projected directly into the
robot model. Some Lite6 firmware reports a zero-filled placeholder while the
drives are disabled and has not yet established the physical joint position.
The historical observer therefore withheld model projection until one
error-free sample had all expected brake/servo bits enabled and the rich
report angles agreed with the polled joint stream within 0.002 rad. It then
latched initialization and republished only structurally valid samples on
`/manyforge/xarm/validated_joint_states`. A later stop could freeze the angles in
the rich status report while the joint stream records mechanical settling;
that divergence was diagnosed explicitly while the validated joint stream
continued to represent the observed pose. None of those observations opened
motion admission.

The captured supervised URDF contract separated fast motion fencing from
terminal session-loss classification. A provisional 1000 ms producer
observation lease bounded both robot-state validity and the internal xArm
write gate; generic admission publication retained its independent 250 ms
lease. If rich reports or successful control reads remained absent for 2000
ms, the xarm_ros2 owner marked that SDK session terminal and required
replacement of the controller-manager process. Reconnecting the cable never
reused or re-armed the old session.

This package and its image are local experimental inputs, not a supported
real-robot lane. The captured production binding selected the supervised
`xarm_ros2` hardware plugin as the sole SDK owner. ManyForge reached its
restricted state/request/result surface only through ROS; the separate
ManyForge hardware plugin was fake/HIL-only. Lifecycle primitives, arming, and
all motion remained later physical gates.

### Historical offline workflow

Build the image from the exact local XC-6 xArm worktree:

```bash
MANYFORGE_XARM_SOURCE_CONTEXT=/path/to/xarm_ros2 \
  ./scripts/xarm/build-real-lite6-image.sh
```

At that baseline, the image build compiled the fork and had to pass the focused
xArm access-policy, driver-lifecycle, supervised-session, and restricted ROS
command-contract C++ tests. It intentionally did not use the fork's
package-wide legacy lint suite as an XC-6 gate.

Generate the fixed-tool model, immutable endpoint/controller binding, and
independent validation result without network access:

```bash
docker run --rm --network none \
  -v "$PWD:/workspace" -w /workspace \
  manyforge-ros-jazzy-real-lite6:experimental \
  ./scripts/ros/generate-lite6-fixed-pneumatic-model.sh
```

The generated binding froze the controller-manager identity, JTC
name/type, action endpoint, joint order, position/velocity command and state
interfaces, update/publication rates, activity QoS, runtime freshness policy,
supervised state/request/gate endpoints, JTC cancel-deceleration contract, and
artifact digests. The focused fake lane loaded the fake-owned equivalent
through the real Jazzy controller manager; the production artifact named the
xarm owner plugin.

At that baseline, the resolved runtime manifest projected those
already-validated facts into
`ros2_control_execution_binding` using the robot-neutral schema
`manyforge.ros2_control.execution_binding.v0.experimental`. This projection
added no inferred prefix, namespace, endpoint, or vendor state: the generic
execution backend consumed exact resolved names and verified them against
controller-manager activity and services. The xArm-specific binding remained
the source of generation identity and artifact provenance.

The captured runtime manifest also carried:

- `execution_admission`, which froze the topic, per-process provider
  identity, supervisor identity, refresh period, and maximum lease; and
- `manyforge_execution_config`, which projected the exact action/state/joint
  endpoints, controller binding, observed-state policy, and mandatory
  admission transport into one ready-to-inject generic execution config; and
- `cell_supervision`, which froze the cell/component dependency graph,
  receiver-local stale budgets, restart domains, and the external
  supervisor's per-start epoch and heartbeat path.

At the captured baseline, the supervised-owner qualification launcher started
the manifest-bound admission provider beside `controller_manager` and
robot-state publication, validated the loaded JTC's read-only
cancel-deceleration parameters before publishing owner readiness, and treated
an unexpected exit of any managed process as a topology failure. A
generated/runtime/live mismatch therefore kept execution admission closed.
This remained an isolated qualification
surface. The former product-shaped `real-robot` launcher consumed
`manyforge_execution_config` and the retained runtime record from a separate,
non-owner Composer service. A host-side cell supervisor observed both
containers and owned bounded dependency-ordered teardown; its arm/shutdown
channel was not writable from either container. Startup remained fail-closed
until the selected headless authority established a lifecycle session.
Credential authentication and the PLC/ROXSIE authority adapter remained
separate release work.

The historical resolver resolved runtime-only values without network access.
It verified the source manifest, template and controller digests, exact token
count, portable identities, authority profile, model equivalence, and every
controller-binding field before writing a new runtime manifest. Only the
robot address and xarm transport-owner ID were substituted into the URDF;
robot-instance and authority identity remained supervisory manifest data:

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

The resolved artifact retained the deployment address by design and therefore
lived only under ignored runtime state. It was not a source/model artifact.

Run the focused package, launch-graph, and lint suite:

```bash
docker run --rm --network none \
  -v "$PWD:/workspace" -w /workspace \
  manyforge-ros-jazzy-real-lite6:experimental \
  ./scripts/ros/test-real-lite6-runtime.sh
```

The historical default launch was disconnected:

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

### Historical physical read-only checkpoint

This historical checkpoint observed one stationary robot. It did not start
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

The historical helper could capture the same gate automatically, with bounded
raw output, return codes, timeouts, postconditions, digests, and host-side
provenance:

```bash
./scripts/xarm/capture-lite6-live-evidence.sh
```

The output was retained under the ignored
`.manyforge-var/real-lite6/live-evidence/` runtime tree. A timeout never
counts as a pass. The validated joint-state observation alone is optional
because a drive-disabled robot may not yet expose a trustworthy physical
pose.

The expected historical graph contained only `/ufactory_driver`,
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

### Historical supervised ROS observation preflight

This historical checkpoint attached to the restricted ROS state surface of an
already-running supervised xarm controller-manager process. It never created
a second SDK session and issued no lifecycle or trajectory command:

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

### Historical attended supervised command evidence

The qualification-only supervised owner launcher started the exact xarm_ros2
owner with the hardware component inactive. It required clean ManyForge
source, the experimental real-Lite6 image label, one literal confirmation,
host networking, host IPC, and no already-labeled owner:

```bash
export MANYFORGE_LITE6_ROBOT_IP=192.168.1.31
export MANYFORGE_LITE6_SUPERVISED_CONFIRM=START_SUPERVISED_OWNER_INACTIVE
./scripts/xarm/launch-lite6-supervised-owner.sh
```

Startup built the four ManyForge ROS packages in the container, generated
and resolved the ignored runtime binding with a fresh provider-session UUID,
then started robot-state publication, controller manager with `lite6_system`
inactive, and the observation-only admission provider. It did not establish
authority, issue a lifecycle primitive, activate hardware/JTC, or send a
trajectory. Keep this terminal attached and stop it with `Ctrl-C`.

Only during an explicitly scheduled attended Stage C row, use a second
terminal to capture one reviewed action:

```bash
export MANYFORGE_LITE6_ACTION=robot.stop_control
export MANYFORGE_LITE6_ACTION_CONFIRMATION=robot.stop_control
./scripts/xarm/capture-lite6-supervised-command.sh
```

The wrapper refused an unlabeled container, private network/IPC topology,
missing runtime binding/install, unknown action, or inexact confirmation. The
installed client routed the action through
`LocalOperatorGateway -> XArmLifecycleRosPort -> restricted xarm_ros2 ROS
service`; it never used the SDK directly. The retained ignored evidence bound
the action/request/session/principal, before/after observations, catalog
postcondition, exact primitive and owner return, gateway audit, timeouts, raw
output digest, image, and source tree. It reported success only after the
observed postcondition and authority closure both passed.

This was not a sequence runner: operators ran one command at a time, inspected
its evidence, and obtained the next physical approval separately. It was also
not the future credential-authenticated Composer/PLC ingress.

The historical Enable and Start Control path additionally armed the then-live
sampler from `lifecycle_transient.py` before the command. It recorded
joint/state initialization and computed deviation only from owner-qualified
poses. If the sampler could not arm, the physical request was not sent. A
missing explicit bound remained visible as pending adjudication rather than
being replaced by a default tolerance.

The separate no-trajectory controller/gate row was:

```bash
export MANYFORGE_LITE6_CONTROLLER_GATE_CONFIRMATION=\
QUALIFY_NO_TRAJECTORY_CONTROLLER_GATE
export MANYFORGE_LITE6_CONTROLLER_GATE_POSE_LIMIT_RAD=0.001
./scripts/xarm/capture-lite6-controller-gate.sh
```

The historical `controller_gate_evidence.py` runner switched only the
binding-selected JTC, never published a trajectory, and bound controller
activity, the 100 ms gate lease, automatic expiry, write/lifecycle counters,
joint samples, and the explicit pose limit into one ignored evidence artifact.
Its historical offline tests qualified the capture path only; physical
promotion still required the attended runbook.

## Current ownership boundary

This package currently owns only its retained offline authoring/qualification
logic and pure helper behavior. It owns no live robot fact or transition.
Responsibility returns in dependency order: retained deployment products at
I3; headless status and alarm projections at I5/H0; addressed components,
resources, and Program Binding at I6/N; caller-neutral lifecycle at I7/L0;
Kernel admission and Stop at I8/P5; and the selected xArm measured-state,
ROS-translation, real-hardware, commissioning, and evidence adapter at I10/R.

At I10/R, the selected vendor adapter must prove that exactly one `xarm_ros2`
owner holds the SDK session and production ros2_control data path. The current
`manyforge_xarm_control` CCLC adapter reaches that owner only through its
restricted ROS boundary; this authoring package must not recreate a second
observer, lifecycle owner, command gate, or transport path.
