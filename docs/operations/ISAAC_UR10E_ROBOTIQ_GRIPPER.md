# Isaac UR10e + Robotiq Gripper Operations

> **Cutover status:** retained as a restoration and behavior reference. The
> command recipes below are not qualified on the current `dev` branch and
> must not be executed until I9/I and its prerequisite headless gates pass.
> Unless explicitly marked as retained adapter logic, present-tense wording
> below describes the captured baseline, not a live Isaac lane.

## Scope

This runbook records the position-commanded Robotiq 2F-140 in the former
ManyForge Isaac UR10e full and light scenes. It does not apply to the Lite6 pneumatic
gripper profile, which uses effort-and-dwell semantics.

The captured program-facing command was the standard
`control_msgs/action/GripperCommand`. Isaac-specific JointState transport is
contained in `manyforge_isaac_gripper_action_bridge`; behavior programs did not
contain simulator-specific gripper nodes.

The C++ ros2_control execution backend owned the ManyForge gripper lifecycle:
admission, progress/dwell, timeout, terminal result, and immutable status. The
Python Isaac bridge is only the simulator transport/action server; it is not a
second ManyForge lifecycle owner.

## Authority Model

The captured qualified path was:

```text
ManyForge command_gripper
  -> /robotiq_gripper_controller/gripper_cmd
  -> Isaac gripper action bridge
  -> /isaac_gripper_state (finger_joint only)
  -> one Isaac OmniGraph articulation controller
  -> finger_joint drive
  -> five PhysX mimic followers
  -> /isaac_joint_states (six-joint completion evidence)
```

Only `finger_joint` is commanded. The five follower joints are mechanical
state owned by PhysX. Sending targets to followers would create competing
control authorities and is forbidden.

An action succeeds only after one coherent, fresh Isaac sample contains all
six joints, every mapped position is within tolerance, and distinct samples
remain matching for the configured settle window. Reaching the master target
alone is not success.

The execution adapter's deployment block also binds the gripper `group_id` and
`joint_name`. It projects accepted gripper command/status into the same
execution-state sink used for arm FK, including Robot Model Authority mimic
joints. Composer therefore renders the canonical robot and command ghost from
backend-owned state. A backend action result is not exposed as completed while
that required projection is pending or failed; the viewer never advances the
gripper on its own.

## Bring-Up

1. Load an immutable Isaac asset release containing the expected full or light
   scene. The Isaac asset repository documents deployment and scene validation.
1. While simulation is stopped, verify the gripper is already aligned with the
   wrist. It must not snap or rotate relative to the wrist when Play begins;
   movement of the complete arm during drive initialization is a separate
   behavior.
1. Start the ManyForge Isaac/ROS lane normally. The UR profile starts the
   position-commanded gripper bridge automatically.
1. Start simulation playback.
1. Verify readiness before running a program:

```bash
docker exec manyforge-isaac-topic-control-1 bash -lc '
  source /opt/ros/jazzy/setup.bash
  ros2 topic echo --once /manyforge/isaac_gripper_bridge/ready
'
```

Readiness is transient-local and fail-closed. It is false until fresh finite
state contains all configured completion joints. Stopping simulation makes it
false after the freshness timeout; restarting playback must make it true again
without restarting the bridge.

## Default UR/Robotiq Contract

The deployed mapping is in
`manyforge_ros_bringup/config/isaac_robotiq_2f_140_gripper_bridge.yaml`:

- command joint: `finger_joint`;
- completion joints: master plus five mimic followers;
- follower signs: `[+1, +1, -1, -1, -1, +1]`;
- tolerance: `0.04 rad`;
- settle time: `0.1 s` over at least three distinct samples;
- feedback freshness: `1.0 s`;
- bridge emergency goal ceiling: `20.0 s`.

The ManyForge C++ gripper owner normally cancels first using the deployment's
motion and result timeout. The bridge ceiling only prevents an orphaned
external action from waiting forever. A timeout or stalled result does not
prove that the fingers are open or closed.

## TCP And Scene State

The USD `ee_link` Xform is not a live rigid-body feedback source. The light
deployments query the rigid `robotiq_arg2f_base_link` through
`simulation_interfaces` and compose the fixed entity-to-`grasp_frame`
transform declared in deployment YAML:

```yaml
measured_tcp:
  entity: /World/ur10e_robotiq2f_140_ROS/ur10e_robotiq2f_140/ee_link/robotiq_arg2f_base_link
  tcp_frame_id: grasp_frame
  frame_id: universe
  entity_to_tcp:
    position_xyz: [0.0, 0.0, 0.2]
    orientation_xyzw: [0.0, 0.0, 0.0, 1.0]
```

The offset is rotated by the observed entity orientation before translation.
All values must be numeric and finite, and the quaternion must be unit length.
This measured TCP is for simulator truth/scene synchronization; it does not
change the commanded-simulation authority rules for arm execution.

## Direct Smoke Test

With playback active and readiness true, send close and open actions:

```bash
docker exec manyforge-isaac-topic-control-1 bash -lc '
  source /opt/ros/jazzy/setup.bash
  ros2 action send_goal --feedback \
    /robotiq_gripper_controller/gripper_cmd \
    control_msgs/action/GripperCommand \
    "{command: {position: 0.55, max_effort: 0.0}}"
'
```

Repeat with `position: 0.0`. Inspect `/isaac_joint_states`; the expected close
vector is approximately `[+0.55, +0.55, -0.55, -0.55, -0.55, +0.55]` in the
configured completion-joint order, and open returns all six to zero.

The acceptance test is the complete light-scene pick/place program: the
gripper must visibly close, the object must leave its support, travel with the
tool, release at the drop location, and fall after release.

The pre-Play pose correction was requalified on 2026-07-19 with immutable
release `gripper-preplay-20260719-r2`. The stopped-to-first-step
wrist-to-gripper relative transform remained within the contract tolerance,
direct close/open converged all six joints, and multiple normal Composer
pick/place passes completed. The scene trace independently recorded a
successful attach, detach at the drop pose, and the released object falling to
its support. Isaac was returned to the standard `STOPPED` state after testing.

## Failure Handling

| Symptom                                           | Interpretation                                                                   | Action                                                                                                               |
| ------------------------------------------------- | -------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------- |
| Readiness remains false                           | Missing/stale/non-finite six-joint state or wrong scene topology                 | Start playback; inspect `/isaac_joint_states`; confirm the immutable scene release                                   |
| Master moves but followers do not                 | Legacy asset or missing PhysX mimic model                                        | Reload the qualified asset; never command followers as a workaround                                                  |
| Goal times out while fingers contact an object    | Linkage did not reach the full geometric target                                  | Treat as stalled evidence; adjust task/grasp or qualified tolerance, not result semantics                            |
| Bridge exits on readiness change                  | Software defect; severity-changing logging was an earlier cause                  | Inspect bridge log; do not bypass supervisor readiness                                                               |
| TCP jumps or stays authored/static                | Non-rigid USD Xform used as measured source                                      | Restore rigid base entity plus explicit entity-to-TCP transform                                                      |
| Gripper is misplaced before Play or snaps on Play | A consuming scene overrode the strongest reusable robot overlay's `ee_link` pose | Load a qualified immutable release; validate that scenes contain no local `ee_link` transform                        |
| Fresh workstation misses gripper geometry         | NVIDIA 5.1 asset cannot resolve and is not cached                                | Restore endpoint access or populate the Isaac resolver cache; readiness must remain false until all six joints exist |
| Isaac resets with a transient controller warning  | Graph reinitialization window                                                    | Accept only if readiness recovers and repeated commands pass; otherwise reload/restart Isaac                         |

Reset does not authorize a second command writer and does not weaken readiness.
If Isaac becomes unresponsive, distinguish stopped simulation from process
failure by calling `/get_simulation_state`, then inspect the latest Kit log and
process before restarting it.

## Change And Qualification Rules

- Do not add follower joints to `command_joint_names`.
- Do not add another articulation controller or raw USD drive writer.
- Keep command mapping separate from completion mapping.
- Keep simulator feedback authoritative for completion in this Isaac profile;
  do not infer completion from the published command.
- Any asset topology, mimic law, force, mapping, TCP, or action-timing change
  requires helper tests, deployment tests, USD contract validation, direct
  close/open, readiness stop/play, and full pick/place qualification.
- The Python bridge is an isolated simulator transport adapter. Production
  hardware remains behind its selected ROS/PLC adapter; behavior semantics and
  evidence requirements stay the same.
