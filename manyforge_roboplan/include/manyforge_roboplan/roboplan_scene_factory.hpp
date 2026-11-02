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

}  // namespace manyforge::roboplan_backend
