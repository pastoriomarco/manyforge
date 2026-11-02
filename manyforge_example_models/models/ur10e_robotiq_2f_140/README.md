# UR10e + Robotiq 2F-140 Example Model

This bundle has no `component.yaml` and isn't placed in a Deployment: it is
the source of the split models `../ur10e` (the arm) and `../robotiq_2f_140` (the
tool), which are placed as separate components with the tool mounted on the
arm's `flange`. It holds their meshes, licenses and upstream sources.

Contents:
- `ur10e_robotiq_2f_140.urdf`
- `ur10e_robotiq_2f_140.structural.urdf`
- `ur10e_robotiq_2f_140.srdf`
- `ur10e_robotiq_2f_140.structural.srdf`
- `meshes/ur10e/*`
- `meshes/robotiq_2f_140/*`

The original URDF and SRDF are retained byte-for-byte as historical/vendor
inputs. The URDF's two `<collision><material>` children are outside ordinary
URDF collision semantics and are ignored by urdfdom's collision parser and
exporter. Its
`.structural.urdf` sibling is mechanically checked to be the exact original
minus only those two elements; inline visual `name=""` color definitions stay
intact. The SRDF's non-standard `<gripper>` element is an HPP Manipulation
extension carrying clearance and grasp-pose metadata; repository history
shows no ManyForge or RoboPlan owner that consumed it. The `.structural.srdf`
sibling is mechanically checked to be the exact original minus that one
extension. ManyForge doesn't use either extension.

Provenance:
- Universal Robots UR10e meshes were copied from the official
  `Universal_Robots_ROS2_Description` repository.
- Robotiq 2F-140 meshes were copied from NVIDIA's `ros2_robotiq_gripper`
  package under the local Isaac ROS workspace.

Licensing:
- See `licenses/Universal_Robots_ROS2_Description.LICENSE`
- See `licenses/ros2_robotiq_gripper.LICENSE`
