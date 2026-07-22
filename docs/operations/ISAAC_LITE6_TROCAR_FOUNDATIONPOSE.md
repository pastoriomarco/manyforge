# Isaac Lite6 Trocar FoundationPose Bring-Up

This runbook is the operator path for the Lite6 trocar pick scenario:

- Isaac Sim runs the Lite6/trocar USD on the simulator host.
- Isaac ROS 4.4 runs YOLOv8 + FoundationPose and publishes
  `vision_msgs/msg/Detection3DArray` on `/output`.
- ManyForge runs its ROS-capable Composer and topic-control services and
  consumes `/output` through a `detection3d_array_topic` pose source.

Do not install ManyForge into the Isaac ROS perception container, and do not
install ManyForge into Isaac Sim. Keep the processes separate and connect them
through ROS 2/DDS.

## Network and DDS Baseline

The known Thor/laptop topology is:

- Thor / ManyForge / Isaac ROS host: `192.168.1.136`
- Isaac Sim laptop: `192.168.1.132`
- ROS domain: `0`
- RMW: Fast DDS (`rmw_fastrtps_cpp`)

Set these in every shell that launches a ROS participant: Isaac Sim on the
laptop, the Isaac ROS 4.4 container on Thor, and the ManyForge launcher shell on
Thor. Set `MANYFORGE_ROOT` to the checkout visible in that shell; do not copy a
user-specific absolute path between hosts.

```bash
export MANYFORGE_ROOT="${MANYFORGE_ROOT:-${HOME}/workspaces/dev_ws/src/manyforge}"
source "${MANYFORGE_ROOT}/config/ros/isaac_lite6_trocar_foundationpose.env.sh"
```

That profile expands to the known-good commissioning environment:
`ROS_DOMAIN_ID=0`, `RMW_IMPLEMENTATION=rmw_fastrtps_cpp`,
`ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST`, and
`ROS_STATIC_PEERS='192.168.1.136;192.168.1.132'`, while unsetting stale Fast
DDS discovery-server/profile and Cyclone DDS variables. If the Thor/Jetson or
laptop IP changes, override `MANYFORGE_THOR_IP` and
`MANYFORGE_ISAAC_SIM_IP` before sourcing the profile. The profile overwrites
stale ROS environment values by design; use `MANYFORGE_ROS_DOMAIN_ID`,
`MANYFORGE_RMW_IMPLEMENTATION`, `MANYFORGE_ROS_AUTOMATIC_DISCOVERY_RANGE`, or
`MANYFORGE_ROS_STATIC_PEERS` only when intentionally qualifying a different
topology.

If the ManyForge checkout is not visible inside a container, mount or copy
`config/ros/isaac_lite6_trocar_foundationpose.env.sh` into that container, or
apply the equivalent values manually:

```bash
unset FASTRTPS_DEFAULT_PROFILES_FILE FASTDDS_DEFAULT_PROFILES_FILE
unset ROS_DISCOVERY_SERVER ROS_SUPER_CLIENT CYCLONEDDS_URI
export ROS_DOMAIN_ID=0
export RMW_IMPLEMENTATION=rmw_fastrtps_cpp
export ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST
export ROS_STATIC_PEERS='192.168.1.136;192.168.1.132'
```

This confines discovery to explicit peers and avoids subnet-wide multicast
discovery. If this is not enough on a given LAN, use the Isaac ROS Fast DDS XML
profile documented in
`isaac_ros_custom_bringup/isaac_ros_4/dds/README.md` as the fallback.

On Thor, set the receive buffer recommended for heavy Isaac ROS streams:

```bash
sudo sysctl -w net.core.rmem_max=2147483647
```

For production stability, prefer a dedicated NIC or dedicated subnet between
the simulator host and Thor. DDS isolation prevents discovery floods; it does
not change the raw bandwidth cost of RGB + depth images.

## Isaac Sim Laptop

Open a fresh shell on the laptop and export the DDS variables before launching
Isaac Sim:

```bash
export MANYFORGE_ROOT="${MANYFORGE_ROOT:-${HOME}/workspaces/dev_ws/src/manyforge}"
source "${MANYFORGE_ROOT}/config/ros/isaac_lite6_trocar_foundationpose.env.sh"
cd ~/isaac-sim
./isaac-sim.sh
```

Load the Lite6 trocar FoundationPose USD and keep playback stopped while
checking the graph. On every `ROS2CameraHelper` node that publishes heavy camera
data, set:

- `qosProfile`:

  ```json
  {"history":"keepLast","depth":1,"reliability":"bestEffort","durability":"volatile","deadline":0.0,"lifespan":0.0,"liveliness":"systemDefault","leaseDuration":0.0}
  ```

- `queueSize`: `1`

- `frameSkipCount`: start high enough to keep the operator network usable.
  Lower it only after the full pipeline is stable.

Apply this to the RGB image (`/image_rect`), depth image (`/depth`), and
`/camera_info` camera info helpers. For industrial cells the camera publisher
must be best-effort/sensor-data style; a reliable raw image publisher can create
retransmit storms when any reliable subscriber or graph tool attaches on a
loaded link.

Press Play only after the DDS environment and QoS are set.

## Isaac ROS 4.4 Perception Container

On Thor:

```bash
export ISAAC_ROS_WS="${ISAAC_ROS_WS:-${HOME}/workspaces/isaac_ros-dev}"
cd "${ISAAC_ROS_WS}"
isaac-ros activate
```

Inside the container:

```bash
cd /workspaces/isaac_ros-dev
source /opt/ros/jazzy/setup.bash
source install/setup.bash 2>/dev/null || true

source /workspaces/manyforge/config/ros/isaac_lite6_trocar_foundationpose.env.sh

colcon build --packages-select isaac_ros_custom_bringup
source install/setup.bash
```

If ManyForge is mounted somewhere else in the Isaac ROS container, source the
same profile from that mount path. If it is not mounted, use the manual exports
from the Network and DDS Baseline section.

Start the perception graph:

```bash
export YOLO_MODEL_NAME=trocar_short
export MESH_FILE_PATH=${ISAAC_ROS_WS}/src/isaac_sim_custom_examples/trocar_short.obj
export TEXTURE_PATH=${ISAAC_ROS_WS}/src/isaac_sim_custom_examples/grey.png

ros2 launch isaac_ros_custom_bringup yolov8_foundationpose_isaac_sim.launch.py \
  yolov8_model_file_path:=${ISAAC_ROS_WS}/isaac_ros_assets/models/yolov8/${YOLO_MODEL_NAME}.onnx \
  yolov8_engine_file_path:=${ISAAC_ROS_WS}/isaac_ros_assets/models/yolov8/${YOLO_MODEL_NAME}.plan \
  force_engine_update:=False \
  input_binding_names:='["images"]' output_binding_names:='["output0"]' \
  num_classes:=1 confidence_threshold:=0.25 nms_threshold:=0.45 \
  mesh_file_path:=${MESH_FILE_PATH} texture_path:=${TEXTURE_PATH} \
  refine_model_file_path:=${ISAAC_ROS_WS}/isaac_ros_assets/models/foundationpose/refine_model.onnx \
  refine_engine_file_path:=${ISAAC_ROS_WS}/isaac_ros_assets/models/foundationpose/refine_trt_engine.plan \
  score_model_file_path:=${ISAAC_ROS_WS}/isaac_ros_assets/models/foundationpose/score_model.onnx \
  score_engine_file_path:=${ISAAC_ROS_WS}/isaac_ros_assets/models/foundationpose/score_trt_engine.plan \
  depth_is_float:=True launch_rviz:=False
```

Use `force_engine_update:=True` only after a TensorRT/GPU image change. Engine
builds are expensive and should not be part of normal operation.

Low-payload checks:

```bash
ros2 topic list --no-daemon --spin-time 20 -t | \
  grep -E 'image_rect|depth|camera_info|detections_output|/output'
ros2 topic info --no-daemon --spin-time 20 -v /output
ros2 topic echo /output vision_msgs/msg/Detection3DArray --once --timeout 120 \
  --qos-reliability reliable --qos-durability volatile
ros2 topic hz /output
```

Avoid `ros2 topic echo` or `ros2 topic hz` on raw image topics while diagnosing
network saturation. They add subscribers to the heavy stream and can change the
problem being measured.

The ManyForge deployment currently uses `stale_after_s: 5.0` for the
FoundationPose source. That budget is intentionally larger than the observed
Isaac ROS 4.4 FoundationPose cadence on Thor/laptop commissioning hardware
(`~0.4 Hz`, one `/output` message every `2.1-2.5 s`). If `/output` slows below
that, treat it as a perception performance fault instead of widening the budget
again. For production, derive this value from the qualified worst-case
FoundationPose period plus bounded scheduling jitter for the specific cell.

## ManyForge Runtime

Start from the ManyForge checkout on Thor. The DDS variables must be exported in
the shell that runs `scripts/launch.sh`; `docker-compose.yaml` passes them into
the ROS-capable services.

```bash
export MANYFORGE_ROOT="${MANYFORGE_ROOT:-${HOME}/workspaces/dev_ws/src/manyforge}"
cd "${MANYFORGE_ROOT}"
source config/ros/isaac_lite6_trocar_foundationpose.env.sh

./scripts/launch.sh --lane isaac-sim --assistant off \
  --scenario isaac-lite6-trocar-foundationpose --non-interactive --yes start
```

For strict qualification:

```bash
./scripts/launch.sh --lane isaac-sim --assistant off \
  --scenario isaac-lite6-trocar-foundationpose --non-interactive --yes smoke
```

The smoke path checks:

- Isaac joint-state visibility.
- Lite6 `FollowJointTrajectory` action.
- Lite6 gripper `GripperCommand` bridge.
- Arm controller state.
- FoundationPose `/output` as a `Detection3DArray` pose source.

The FoundationPose smoke validator deliberately passes
`--allow-zero-confidence` and `--allow-zero-covariance` because the currently
observed Isaac ROS `/output` publishes empty id/class, score `0.0`, and zero
covariance. This is a bootstrap allowance, not a production perception gate.

## Frame Contract

Observed Isaac ROS FoundationPose output is in `camera_color_optical_frame`.
ManyForge needs the trocar object in the Lite6 planning frame `link_base`.
The deployment sets:

```yaml
pose_sources:
- id: foundationpose_trocar
  kind: detection3d_array_topic
  topic: /output
  frame_id: link_base
  stale_after_s: 5.0
  objects:
    graspable:
      apply_policy: before_planning
      confidence_min: 0.0
      metadata:
        selection: first
        max_position_variance_m2: 0.0025
```

The ROS sidecar performs a TF lookup from `link_base` to the message frame and
transforms the pose before publishing the snapshot to Composer.

Check the TF chain before trusting motion:

```bash
ros2 run tf2_ros tf2_echo link_base camera_color_optical_frame
ros2 run tf2_ros tf2_echo universe link_base
```

Existence is not enough. Before a real pick attempt, compare the transformed
trocar pose against Isaac Sim ground truth for the trocar object. A plausible
but inaccurate camera transform will produce a plausible but wrong pick pose.

## Plan Pick Authoring And Execution

`Plan Pick Sequence` is a no-motion planning node. It reads the selected
detached object from the backend-authoritative scene, synthesizes bounded pick
and approach poses, and validates the three planning legs before publishing a
`pick_plan_v1` blackboard bundle. It does not close the gripper, attach the
object, or execute an arm trajectory.

In Composer, **Pick Plan Subtree** inserts the ordinary editable nodes needed
for the pick half of the workflow:

```text
Sequence
  Plan Pick Sequence
  Sequence
    Move to approach
    Cartesian descend to pick
    Close gripper
    Attach selected object
    Cartesian retreat to approach
```

Insertion requires exactly one active deployment gripper and one selected,
detached scene object. Failure is atomic: Composer adds no partial nodes or
blackboard declarations. The generated nodes remain fully visible and editable;
the template itself is never saved or run. Repeated insertions receive one
deterministic suffix across node names and the generated blackboard namespace.

The approach move is an absolute pose motion. Descend and retreat are
fail-closed Cartesian paths and require the deployment to advertise
`planning.motion.cartesian_path`. Each generated motion has independent
selectors for motion type (`joint`, `pose`, `cartesian`), target mode
(`absolute`, `relative`), and planner-neutral planning profile
(`first_feasible`, `balanced`, `quality_bounded`). Relative describes how a
target is resolved; it is not a motion type. Unsupported choices are rejected
instead of silently falling back to another planner behavior.

When a stage fails, inspect the Plan Pick report and the failing motion node.
The report retains the requested stage target/path and identifies whether pose
synthesis, planning, collision validation, support-contact release, or another
authoritative gate failed. A failed Cartesian descend or retreat is not retried
as endpoint-only pose planning.

Use a narrow `contact_grasp` policy only for the intended tool/object contact.
After attachment, `touch_links` are the only gripper/object collision allowance.
Support-contact release remains a separate object/support policy and may not be
replaced by a broad collision exemption. Preview and execution must use the same
object, frames, contact policy, scene lease, and validation path.

Program artifacts persist joint angles and angular pose fields in radians.
Composer may display degrees for operator convenience, but it converts back to
radians before applying or saving the program.

## Current Bootstrap Limits

The current `/output` evidence from Isaac ROS 4.4 is usable for wiring but weak
for production gating:

- `header.frame_id`: `camera_color_optical_frame`
- `detections[0].id`: empty
- `results[0].hypothesis.class_id`: empty
- `results[0].hypothesis.score`: `0.0`
- `pose.covariance`: all zeros

Because of that, the deployment uses `metadata.selection: first` and
`confidence_min: 0.0`. This is acceptable only while a single trocar is present
and the scene is controlled. Before industrial use, restore nonzero perception
quality signals or add a separately qualified gate that proves the selected pose
is the intended object.

The program also enables `plan_pick_sequence.force_vertical_pick: true`,
`enable_min_tcp_z: true`, and `min_tcp_z_m: 0.016`. This is a planner-side
top-down grasp constraint: the generated pick and approach TCP poses force the
local TCP Z axis downward while preserving the selected object's horizontal
heading, and clamp generated TCP Z above the configured floor before preview
validation. `enable_min_tcp_z` can be switched off to keep the threshold value
visible in Composer without enforcing the clamp. It does not replace a future
pose-source conditioning policy for cleaning the scene object's stored
orientation.

The pick planner's pose-construction order is intentional:

1. Read the selected object's backend-authoritative pose in the planning base
   frame.
1. Anchor the nominal pick point to the selected object and its top surface.
1. Compute the candidate pick TCP orientation. With `force_vertical_pick` true,
   the TCP local Z axis is forced vertical-down while preserving the object's
   horizontal heading; the object pose itself is not moved or re-oriented.
1. Apply `pick_offset_envelope` XYZ modifiers in `pick_offset_reference`.
1. Apply `min_tcp_z_m` if enabled; this happens after the reference-basis
   offset and can change the final TCP Z from the pure rotated offset.
1. Apply `approach_offset_envelope` XYZ modifiers relative to the final pick
   pose in `approach_offset_reference`.

`pick_offset_envelope` and `approach_offset_envelope` XYZ values are expressed
in their paired reference parameters: `pick_offset_reference` and
`approach_offset_reference`. This scenario currently pins both to
`robot_link: link_tcp`: the pick offset `[-0.015, 0.0, 0.004]` and approach
offset `[0.0, 0.0, -0.08]` are expressed in the generated pick TCP basis, not
the robot's current live TCP pose before planning. If an offset should follow
the detected trocar heading, set the corresponding reference to `object: graspable`; if it should stay in the planning base, set it to `frame: link_base`. The vertical-pick option rectifies orientation only; it does not
override the selected translation reference.

Robot Control Cartesian candidates preserve explicit frame parity. A manual
Move To preview planned in `link_base` for TCP `link_tcp` must approve and
execute with the same `base_frame`, `tcp_frame`, and `pose_goal.frame_id`.
Custom frame/TCP testing is allowed only after the frame inventory and TF chain
have been verified; do not treat a successful preview in one frame as evidence
that approval is safe in another.

The `graspable` scene resource declares a support-contact release policy against
`ground`, with `max_start_penetration_m: 0.03` while the first-waypoint cap is
enabled. This lets the attached trocar start the lift in bounded contact with
the support surface, but validation still fails closed unless the retreat
trajectory separates that exact pair and does not re-contact. Do not replace
this with a global collision exemption; only the picked-object/support pair
should receive this policy.

In Composer, edit these relationships from **Scene -> Support Contacts**. The
section only offers authored scene objects, rejects duplicate object/support
pairs, and prompts before deleting an object that participates in a support
contact. `Limit start penetration` maps to
`enforce_max_start_penetration`; disabling it removes the first-waypoint
penetration envelope only. Release, final separation, and re-contact checks
remain active.

If a validation banner shows a compressed pair such as `ground <-> 0`, expand
the diagnostics. A `collision_object_pair_N` entry of `graspable <-> ground`
means the raw RoboPlan body name was resolved to the configured support-contact
pair and the support-contact state machine will evaluate it. If no object-pair
entry is present, the other body is not the supported object, for example a
robot link; support contacts must not hide that collision.

## Troubleshooting

If LAN/internet latency spikes only when Isaac Sim and FoundationPose are both
running, reduce raw camera rate first and verify the camera helpers use
best-effort QoS. Then verify the DDS env vars are present in all three
execution contexts.

If `ros2 topic list --no-daemon` sees `/output` but `ros2 topic echo /output`
does not, provide the type and QoS explicitly:

```bash
ros2 topic echo /output vision_msgs/msg/Detection3DArray --once --timeout 120 \
  --qos-reliability reliable --qos-durability volatile
```

If Composer reports `no fresh pose-source update for: graspable`, check the
perception rate before pressing Run:

```bash
ros2 topic hz /output
```

The scenario is sized for `/output` periods up to roughly `5 s`. A 1 Hz or
faster stream should stay fresh continuously. A `~0.4 Hz` stream will now remain
fresh long enough for the before-planning pose apply step, but it is still a
commissioning-grade perception rate, not a production throughput target.

If ManyForge smoke fails with `tf_lookup_failed`, the perception stream is
arriving but the `link_base <- camera_color_optical_frame` transform is missing
or not visible to the ManyForge ROS sidecar. Fix TF before running motion.

If the arm or gripper actions are missing, confirm the scenario is
`isaac-lite6-trocar-foundationpose`. The launcher sets the Lite6 topic-control
profile and gripper action defaults for this scenario.
