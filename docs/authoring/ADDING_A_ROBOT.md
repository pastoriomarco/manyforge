# Adding a robot (Mock)

A robot is data: a model bundle, a model family row, a component profile row
and a Mock controller row. Adding one needs no C++ edit, but it needs a
rebuild, because the rows are compiled into the workbench. This guide covers
the **Mock lane only**. Isaac and Real robots still need code.

All commands run in the workbench container from the repository root; ROS is
never installed on the host. A rebuild is
`./scripts/compose-dev.sh stop && ./scripts/compose-dev.sh start`.

UFACTORY models (xArm 5/6/7, Lite 6, UF850) share one controller template, and
so do UR models. A robot of another vendor family needs a new controller
template, which is code.

## What you need

- The robot description: URDF (xacro expanded once) with meshes, and a MoveIt
  SRDF with an arm group and, for a gripper, a gripper group with `open` and
  `closed` group states.
- The vendor controller file, for the joint trajectory and goal constraints.
- The vendor TCP speed limit. A Deployment's TCP ceiling must not exceed it.

## Steps

The examples use the UF850 with the xArm gripper (`uf850_xarm_gripper`),
whose rows ship with ManyForge.

1. **Model bundle.** Put the files in
   `manyforge_example_models/models/<family>/`: `<family>.urdf`,
   `<family>.srdf`, `meshes/`, `licenses/` and a `README.md` saying where the
   files come from. Every mesh URI must be
   `package://manyforge_example_models/models/<family>/...`. Keep joint names
   and mimic joints as upstream.

2. **Model family row.** Append one row to `model_families` in
   `manyforge_deployment/resources/maintained/maintained_deployment_sources.v1.json`
   (copy the `uf850_xarm_gripper` row; keep rows sorted by id). It names the
   bundle (`source`: URDF, SRDF, `resource_root`, the package prefix; both
   `*_projection_kind` values `identity` and the `historical_*` paths equal to
   the plain ones) and the roles ManyForge uses:
   - frames `base`, `flange`, `tcp`, `gripper_tcp` and `root`;
   - groups `arm` and, with a gripper, `gripper` (SRDF group names);
   - joints `arm_joints` (base to tip) and, with a gripper, `fingers` (the one
     commanded gripper joint, not a mimic follower);
   - collision links `arm_collision` and `gripper_collision`.

3. **Component profile row.** Append a row to `runtime_restoration_profiles`
   in the same file, sorted by id:

   ```json
   {
     "runtime_restoration_profile_id": "<family>_mock_commanded_v0",
     "model_family_id": "<family>",
     "ros2_control_runtime": {"mock_profile": "<family>"},
     "source_fields": {},
     "stop_profiles": {},
     "acceptance_obligations": []
   }
   ```

4. **Mock controller row.** Append a row to
   `manyforge_ros_bringup/config/mock_controller_profiles.yaml`:

   ```yaml
     - mock_profile: uf850_xarm_gripper
       template: ufactory                 # ufactory or ur
       hardware_component_name: MockUF850XArmGripperSystem
       arm_controller: uf850_traj_controller
       vendor_joint_constraints: {trajectory: "1.0", goal: "0.01"}  # ufactory only
       gripper_controller: xarm_gripper_controller   # only with a `fingers` role
       hard_stop_completion_timeout_s: 1.5
       gripper_result_timeout_s: 5.0
   ```

   Copy `vendor_joint_constraints` from the vendor controller file
   (`xarm_controller/config/<model>_controllers.yaml`); leave it out for `ur`.
   The build validates this file and stops with the row and field at fault.

5. **Generate the component types.** After one workbench start (which
   configures `/runner/build/deployment-authoring`):

   ```bash
   cmake --build /runner/build/deployment-authoring --target manyforge_deployment_revision
   PYTHONPATH=manyforge_behavior:. python3 manyforge_deployment/tools/generate_maintained_deployment_occurrences.py \
     --update \
     --description manyforge_deployment/resources/maintained/maintained_deployment_sources.v1.json \
     --assistant-profiles manyforge_deployment/resources/maintained/maintained_assistant_profiles.v1.json \
     --workspace-root . --revision-tool /runner/build/deployment-authoring/manyforge_deployment_revision
   ```

   It writes `<family>.component.json` and `<family>.fixed_scale.component.json`
   under `manyforge_deployment/resources/maintained/catalog/component_type/`
   and changes nothing else (check with `git status`).

6. **Accept the id in the runtime schema.** The closed runtime schema
   (`manyforge_deployment/resources/closed_schema/value/manyforge.value_schema.runtime.ros2_control_execution.v1.yaml`)
   still lists the accepted `mock_profile` ids, and so does
   `manyforge_deployment/src/ros2_control_execution_runtime_unit_internal.cpp`.
   A new id must be added to both. This edit changes the schema's revision,
   which is recorded in several files, and every ROS component's identity, so
   it is a maintainer change, not part of a site setup.

7. **Scenario.** Copy a Mock example Scenario (for example
   `examples/lite6_pick_and_place.scenario`) and change its Deployment
   component: `component_profile_id`, `runtime_configuration.mock_profile`,
   `model_source` (the bundle's URDF and SRDF), `initial_joint_positions_rad`
   (one value per arm joint), the Stop profiles (one entry per arm joint), the
   gripper row (frames, touch links, `open`/`closed` states) and the TCP
   ceiling (at or below the vendor limit; start at 0.15 m/s). Do not edit
   `scenario.yaml` digests; Save Scenario rewrites them.

8. **Check offline:**

   ```bash
   python3 scripts/robots/check_robot.py manyforge_example_models/models/<family> \
     --scenario examples/<name>.scenario
   ```

   It joins the bundle, the family row, the profile row, the Mock controller
   row and the Scenario, and prints one `error: <where>: <what>` line per
   problem, for example:

   ```text
   error: model family uf850_xarm_gripper: frame role 'tcp': link 'link_tcpx' is not in uf850_xarm_gripper.urdf
   error: Mock controller row uf850_xarm_gripper: the family has a 'fingers' role, so the row needs gripper_controller
   ```

9. **Rebuild and load.** Rebuild, then start with the Scenario
   (`./scripts/compose-dev.sh start --project examples/<name>.scenario`). The
   start reports the Scenario `ready`. In Composer, check that the model and
   gripper render, jog each joint, open and close the gripper, and run the
   Program.

## Messages at Apply

| Message | Cause |
| --- | --- |
| `component X selects unknown profile Y` | The profile row is missing or misspelled. |
| `selects unknown model family` | The family row is missing. |
| `mock_profile does not match its ROS component profile` | The Deployment and the profile row name different `mock_profile` ids. |
| `prepared model differs from its repository-owned profile` | A mesh URI or package root does not use the family's package prefix. |
| `unsupported mock controller profile` | The Mock controller row is missing. |
| `component_streams must declare exactly measured_joints and gripper_feedback` | The streams do not match the gripper. |
