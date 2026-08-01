# UR10e + Robotiq 2F-140 Example Model

> **Current boundary:** the static files and provenance remain valid reference
> assets, but no Isaac or native product lane is currently available. Keeping
> the bundle self-contained does not qualify a viewer, planner, or simulator
> integration.

This preview model bundle was vendored into `manyforge_example_models` to make
the captured ManyForge Isaac/preview path self-contained.

Contents:
- `ur10e_robotiq_2f_140.urdf`
- `ur10e_robotiq_2f_140.srdf`
- `meshes/ur10e/*`
- `meshes/robotiq_2f_140/*`

Provenance:
- Universal Robots UR10e meshes were copied from the official
  `Universal_Robots_ROS2_Description` repository.
- Robotiq 2F-140 meshes were copied from NVIDIA's `ros2_robotiq_gripper`
  package under the local Isaac ROS workspace.

Licensing:
- See `licenses/Universal_Robots_ROS2_Description.LICENSE`
- See `licenses/ros2_robotiq_gripper.LICENSE`

This bundle is a preview/demo enabler, not yet a long-term model packaging
architecture for ManyForge.
