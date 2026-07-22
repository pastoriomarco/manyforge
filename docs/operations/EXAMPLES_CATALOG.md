# Examples Catalog — Program × Deployment Pairs

This doc catalogues every `examples/*.program.yaml` and
`examples/*.deployment.yaml` file and maps them to the scenarios the
unified launcher (`scripts/launch.sh`) exposes. Use it to pick the right
combination for a given test or demo.

For the launcher itself, see
[`LAUNCHER_QUICKSTART.md`](LAUNCHER_QUICKSTART.md). For the live source
of truth on each scenario, see `compatibility.scenarios` in its deployment
artifact. `scripts/lib/scenarios.sh` only loads and presents those discovered
definitions.

## How to pick a combination

The launcher takes three axes: **lane**, **assistant**, and **scenario**.
A scenario is a fixed pairing of a program YAML and a deployment YAML
plus a lane/assistant default. To use an ad hoc combination not in the table,
pass both `--program PATH` and `--deployment PATH`. To publish a reusable named
scenario, add `compatibility.scenarios` metadata to the deployment artifact.

The compatibility rule is simple: a program and deployment must agree on
`robot_id`, `group_id`, required profile, and capabilities. Artifact discovery
validates scenario metadata and paths; program/deployment load then fails
closed on an incompatible pair (inspect `GET /api/cycle/status`; see
[`TROUBLESHOOTING.md`](TROUBLESHOOTING.md)).

Three operator-facing axes determine which scenario to pick:

- **Robot model.** UR5 (`pick_and_place.deployment.yaml`,
  `semantic_shape_pick.deployment.yaml`, `vision_guided_*.deployment.yaml`)
  is the default for self-contained simulated demos. UR10e + Robotiq
  (`pick_and_place_ur10e_robotiq.deployment.yaml`,
  `assistant_modes_scene_authoring.deployment.yaml`,
  `isaac_manipulator_scene_light_*.deployment.yaml`) is the canonical
  manipulator-with-gripper target and the Isaac lane's robot. UFACTORY
  Lite6 + lite gripper (`lite6_lite_gripper_pick_and_place.*`,
  `isaac_lite6_lite_gripper_pick_and_place.*`) is the compact-arm
  bring-up target for the Robot Model Authority pipeline.
- **Execution backend.** `execution.roboplan_native` for native
  simulated execution; `execution.ros2_control_follow_joint_trajectory`
  for the Isaac lane (commanded-sim, FJT action server,
  ros2_control bridge). Real-robot execution uses the same
  ros2_control adapter with `disable_controller_path_tolerances: false`
  — see [`STACK_SETUP.md`](STACK_SETUP.md).
- **Assistant.** Off for deterministic / qualification runs; on
  (`openclaw` provider) for any scenario whose program uses
  `query_model` or `assistant.chat` nodes. Assistant-on deployments
  must declare `model_endpoint_profile_registry: model_endpoint_profiles.yaml`
  and reference profiles via `model_endpoints.<alias>.profile_ref`.

## Catalogued scenarios

| Scenario                            | Lane           | AI  | Program                                                                                                                       | Deployment                                                                                                                                                    | What it demonstrates                                                                                                                                                                                                                                                       |
| ----------------------------------- | -------------- | --- | ----------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `ur5-pick-place`                    | manyforge-only | off | [`pick_and_place.program.yaml`](../../examples/pick_and_place.program.yaml)                                                   | [`pick_and_place.deployment.yaml`](../../examples/pick_and_place.deployment.yaml)                                                                             | UR5 + RoboPlan baseline (12-step approach/pick/retreat/drop). The default smoke target.                                                                                                                                                                                    |
| `ur5-simple-pick-place`             | manyforge-only | off | [`pick_and_place_minimal.program.yaml`](../../examples/pick_and_place_minimal.program.yaml)                                   | [`pick_and_place.deployment.yaml`](../../examples/pick_and_place.deployment.yaml)                                                                             | UR5 3-step minimal fixture (was `demo.sh --simple`). Quickest scenario sanity check.                                                                                                                                                                                       |
| `ur10e-pick-place`                  | manyforge-only | off | [`pick_and_place_ur10e_robotiq.program.yaml`](../../examples/pick_and_place_ur10e_robotiq.program.yaml)                       | [`pick_and_place_ur10e_robotiq.deployment.yaml`](../../examples/pick_and_place_ur10e_robotiq.deployment.yaml)                                                 | UR10e + Robotiq 2F-140 self-contained preview. Exercises the gripper command/feedback capability keys (`tool.gripper.command`, `tool.gripper.feedback`).                                                                                                                   |
| `lite6-pick-place`                  | manyforge-only | off | [`lite6_lite_gripper_pick_and_place.program.yaml`](../../examples/lite6_lite_gripper_pick_and_place.program.yaml)             | [`lite6_lite_gripper_pick_and_place.deployment.yaml`](../../examples/lite6_lite_gripper_pick_and_place.deployment.yaml)                                       | UFACTORY Lite6 + lite gripper native pick-and-place. Uses the canonical Lite6 model snapshot and a compact box scene sized for the shorter Lite6 reach.                                                                                                                    |
| `ur5-semantic-shape-pick`           | manyforge-only | on  | [`semantic_shape_pick.program.yaml`](../../examples/semantic_shape_pick.program.yaml)                                         | [`semantic_shape_pick.deployment.yaml`](../../examples/semantic_shape_pick.deployment.yaml)                                                                   | UR5 semantic shape pick. Deterministic node builds a selection manifest, vision model picks one candidate, deterministic validator confirms before planning. Uses `thor_manyforge_qwen36` for selection + `thor_cosmos_reason2_8b` for chat.                               |
| `ur5-vision-pose-pick`              | manyforge-only | on  | [`vision_guided_pose_pick.program.yaml`](../../examples/vision_guided_pose_pick.program.yaml)                                 | [`vision_guided_pose_pick.deployment.yaml`](../../examples/vision_guided_pose_pick.deployment.yaml)                                                           | UR5 vision-guided pose pick. `query_model` returns validated `approach_pose` / `grasp_pose` in robot base frame; downstream `move_manipulator_action` nodes consume via typed blackboard refs. Requires a live viewer snapshot provider.                                   |
| `ur10e-scene-authoring`             | manyforge-only | on  | [`pick_and_place_ur10e_robotiq.program.yaml`](../../examples/pick_and_place_ur10e_robotiq.program.yaml)                       | [`assistant_modes_scene_authoring.deployment.yaml`](../../examples/assistant_modes_scene_authoring.deployment.yaml)                                           | UR10e + Robotiq with the full Cosmos assistant + scene authoring deployment. The canonical assistant-on deployment that other AI deployments copy from. `composer-assistant.authority.review: required`.                                                                   |
| `isaac-ur10e-light-scene`           | isaac-sim      | off | [`isaac_manipulator_scene_light.program.yaml`](../../examples/isaac_manipulator_scene_light.program.yaml)                     | [`isaac_manipulator_scene_light_ros2_control.deployment.yaml`](../../examples/isaac_manipulator_scene_light_ros2_control.deployment.yaml)                     | Isaac commanded-sim with UR10e light manipulator scene. FJT action `/isaac_arm_controller/follow_joint_trajectory`, `pose_sources.isaac_scene_objects` with `apply_policy: continuous_unattached`, scene meshes from `examples/isaac_manipulator_scene_light/meshes/`.     |
| `isaac-ur10e-light-scene-collision` | isaac-sim      | off | [`isaac_manipulator_scene_light_collision.program.yaml`](../../examples/isaac_manipulator_scene_light_collision.program.yaml) | [`isaac_manipulator_scene_light_ros2_control_collision.deployment.yaml`](../../examples/isaac_manipulator_scene_light_ros2_control_collision.deployment.yaml) | Isaac commanded-sim with the UR10e light scene plus a popup collision object for runtime-fault testing.                                                                                                                                                                    |
| `isaac-lite6-pick-place`            | isaac-sim      | off | [`isaac_lite6_lite_gripper_pick_and_place.program.yaml`](../../examples/isaac_lite6_lite_gripper_pick_and_place.program.yaml) | [`isaac_lite6_lite_gripper_pick_and_place.deployment.yaml`](../../examples/isaac_lite6_lite_gripper_pick_and_place.deployment.yaml)                           | Isaac commanded-sim with UFACTORY Lite6 + lite gripper pick-and-place. Uses the Lite6 topic-control profile, lite gripper action bridge, and the same compact pick/drop scene as the native Lite6 example.                                                                 |
| `isaac-lite6-trocar-foundationpose` | isaac-sim      | off | [`trocar_lite6_foundationpose_pick.program.yaml`](../../examples/trocar_lite6_foundationpose_pick.program.yaml)               | [`trocar_lite6_foundationpose_pick.deployment.yaml`](../../examples/trocar_lite6_foundationpose_pick.deployment.yaml)                                         | Isaac commanded-sim with UFACTORY Lite6 + lite gripper using an Isaac ROS FoundationPose `Detection3DArray` pose source on `/output`. See [`ISAAC_LITE6_TROCAR_FOUNDATIONPOSE.md`](ISAAC_LITE6_TROCAR_FOUNDATIONPOSE.md) for DDS, QoS, perception, TF, and smoke workflow. |
| `isaac-ur10e-light-scene-assistant` | isaac-sim      | on  | [`isaac_manipulator_scene_light.program.yaml`](../../examples/isaac_manipulator_scene_light.program.yaml)                     | [`isaac_manipulator_scene_light_assistant.deployment.yaml`](../../examples/isaac_manipulator_scene_light_assistant.deployment.yaml)                           | Isaac commanded-sim with the full Cosmos assistant overlay. `apply_policy: before_planning` (instead of `continuous_unattached`) because commanded_sim makes assistant edits authoritative for what Isaac sees.                                                            |

Deprecated scenario aliases (still work, emit a one-time warning):
`pick-place`, `ur10e`, `shape-pick`, `vision-pose`, `assistant-authoring`,
`isaac-light`, `isaac-assistant`. Their mappings live beside the canonical
scenario definitions in deployment metadata.

## Other files in `examples/` (not in any scenario)

These exist for purposes outside the launcher's scenario flow — they
are not orphans, just not bound to a launcher entry:

- [`examples/empty_pick_and_place_ur10e_robotiq.program.yaml`](../../examples/empty_pick_and_place_ur10e_robotiq.program.yaml)
  - Empty program scaffold for the `smoke_corpus.yaml` PnP build chain.
    No scene, tree, parameters, or blackboard — the assistant builds
    everything via the `PnP_01 → PnP_20` prompt sequence. Pair with
    `pick_and_place_ur10e_robotiq.deployment.yaml` (or
    `assistant_modes_scene_authoring.deployment.yaml`) when driving
    from the assistant lane.
- [`examples/vision_guided_bin_pick.program.yaml`](../../examples/vision_guided_bin_pick.program.yaml)
  - [`examples/vision_guided_bin_pick.deployment.yaml`](../../examples/vision_guided_bin_pick.deployment.yaml)
  * UR5 left/right symmetric bin variant of the vision demos. The
    `query_model` picks one of two candidate objects; the tree routes
    into a fixed pick/place per the validated decision. Currently not
    bound to a scenario — pass both files through launcher `--program` /
    `--deployment`, or add deployment scenario metadata.
- [`examples/model_endpoint_profiles.yaml`](../../examples/model_endpoint_profiles.yaml)
  - Shared registry of vLLM-served model endpoints. Three profiles
    today: `thor_manyforge_qwen36`, `thor_cosmos_reason2_8b`,
    `thor_nemotron3_nano_omni`. Deployments reference profiles via
    `model_endpoint_profile_registry: model_endpoint_profiles.yaml` +
    `model_endpoints.<alias>.profile_ref`.

## Qualification fixtures (not in `examples/`)

For completeness — these are not operator scenarios, but they are
program/deployment files referenced by the qualification harness and
may appear in logs:

- `verification/programs/native_simple_motion_ur5.program.yaml` +
  `verification/deployments/native_simple_motion_ur5.deployment.yaml`
  - Driven by `verification/matrices/*.yaml` through
    `scripts/run-scenarios.py` and `scripts/qualify-*.sh`. See the
    diagnostic-scripts table in
    [`OBSERVABILITY.md`](OBSERVABILITY.md).
- `manyforge_behavior/tests/fixtures/pick_and_place.program.yaml`,
  `looping_program.program.yaml`, `test.deployment.yaml`,
  `ur5_roboplan.deployment.yaml`
  - Test fixtures consumed by the pytest suites. `ur5-simple-pick-place`
    pulls the first one as a launcher scenario program.

## Adding a new scenario

To wire a new program/deployment pair into the launcher:

1. Drop both YAMLs under `examples/`. Confirm `robot_id` and
   `group_id` match between them.
1. Add a `compatibility.scenarios` entry to the deployment with a unique
   canonical name, program path, description, and optional deprecated aliases.
   The deployment's `compatibility.lane` and assistant configuration determine
   launcher filtering.
1. Verify with `./scripts/launch.sh --list-scenarios`.
1. Smoke with `./scripts/launch.sh --scenario <name> smoke`.

If the deployment uses the assistant lane, also reference
`model_endpoint_profiles.yaml` for any vLLM endpoints — do not inline
endpoint URLs in the deployment when an existing profile fits.
