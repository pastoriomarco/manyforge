#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace roboplan {
class Scene;
}

namespace manyforge::roboplan_backend {

/// One compiler-derived joint-limit row for a verified retained model.
/// Values use SI units: radians or metres, seconds, and seconds squared.
struct RoboPlanJointLimitInput final {
  std::string joint_name;
  std::optional<std::vector<double>> minimum_position;
  std::optional<std::vector<double>> maximum_position;
  std::vector<double> maximum_velocity;
  std::optional<std::vector<double>> maximum_acceleration;

  friend bool operator==(const RoboPlanJointLimitInput &,
                         const RoboPlanJointLimitInput &) = default;
};

/// Loader-only view derived from one verified retained asset closure.
///
/// The verified boundary accepts closure-local asset:// references. The retained
/// stage owns the bounded rewrite into urdf_xml and package_paths for RoboPlan;
/// callers must not supply ambient source-tree or package lookup roots here.
struct RoboPlanSceneInput final {
  std::string scene_name;
  std::string urdf_xml;
  std::string srdf_xml;
  std::vector<std::filesystem::path> package_paths;
  std::vector<RoboPlanJointLimitInput> joint_limits;
};

/// Builds a RoboPlan 0.8 Scene in the required order:
/// description, explicit retained limits, then SRDF semantics.
///
/// Throws std::runtime_error on incomplete, duplicate, foreign, malformed, or
/// not-applied input. Logical Scene and collision authority remain outside
/// this compatibility adapter.
[[nodiscard]] std::shared_ptr<roboplan::Scene> make_roboplan_scene(
    RoboPlanSceneInput input);

/// Request-local diagnostic scene with retained kinematics and no collision pairs.
[[nodiscard]] std::shared_ptr<roboplan::Scene> make_collision_disabled_scene(
    const roboplan::Scene &source, const std::string &scene_name,
    const std::vector<RoboPlanJointLimitInput> &joint_limits,
    const std::string &srdf_xml);

/// Removes every collision pair of `scene` in which neither geometry moves
/// with `group_joint_names` (OD-359): a robot plans and checks only pairs
/// with at least one of its own bodies (its links, its tools, objects
/// attached to them). Pairs between other components' bodies and the cell's
/// fixed objects are governed by those components' own rules and checks.
/// Uses the collision engine's own-body test. Call after every geometry is
/// placed; throws std::runtime_error when the scene refuses the change.
void ignore_pairs_without_group_bodies(
    roboplan::Scene &scene, const std::vector<std::string> &group_joint_names);

/// Sets the scene's current positions to `base_positions` (one value per
/// model coordinate) with the named scalar joints at `positions`:
/// `toFullJointPositions(group, q)` and the planners then see every joint
/// outside the group there. With no names the scene is at `base_positions`
/// again, so nothing of an earlier call remains. Throws std::runtime_error
/// when the counts differ or a name is absent or not a scalar joint.
void set_scene_joint_positions(roboplan::Scene &scene,
                               const std::vector<double> &base_positions,
                               const std::vector<std::string> &joint_names,
                               const std::vector<double> &positions);

}  // namespace manyforge::roboplan_backend
