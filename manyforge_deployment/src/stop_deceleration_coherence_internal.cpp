#include "stop_deceleration_coherence_internal.hpp"

#include <cmath>

#include <manyforge_deployment/canonical_value.hpp>

namespace manyforge::deployment::detail {
namespace {

// Shortest round-trip (RFC 8785) spelling, so the message repeats the
// authored value exactly ("4", "2.5"), never a padded or rounded one.
[[nodiscard]] std::string number_text(double value) {
  return canonical_json(CanonicalValue(value));
}

} // namespace

std::optional<double> planning_joint_acceleration_limit_rad_s2(
    const UrdfDocumentIr &urdf, std::string_view joint_name) {
  for (const auto &joint : urdf.joints) {
    if (joint.name != joint_name) continue;
    if (joint.type == UrdfJointType::kFixed ||
        joint.type == UrdfJointType::kFloating ||
        joint.type == UrdfJointType::kPlanar || joint.mimic.has_value() ||
        !joint.limit.has_value() || !joint.limit->acceleration.has_value()) {
      return std::nullopt;
    }
    const double acceleration = *joint.limit->acceleration;
    if (!std::isfinite(acceleration) || acceleration <= 0.0) {
      return std::nullopt;
    }
    return acceleration;
  }
  return std::nullopt;
}

std::optional<std::string> stop_deceleration_coherence_error(
    std::string_view stop_profile_label, std::string_view joint_name,
    const double stop_max_deceleration_rad_s2,
    const std::optional<double> planning_acceleration_limit_rad_s2) {
  if (!planning_acceleration_limit_rad_s2.has_value() ||
      !(stop_max_deceleration_rad_s2 < *planning_acceleration_limit_rad_s2)) {
    return std::nullopt;
  }
  const auto stop = number_text(stop_max_deceleration_rad_s2);
  const auto planning = number_text(*planning_acceleration_limit_rad_s2);
  return "Stop profile " + std::string(stop_profile_label) + ", joint " +
         std::string(joint_name) + ": Stop deceleration limit " + stop +
         " rad/s^2 (max_deceleration_rad_s2) is lower than the normal-motion "
         "planning acceleration limit " + planning +
         " rad/s^2 (robot model joint acceleration limit). A Stop must be able "
         "to decelerate at least as hard as normal motion accelerates; set the "
         "Stop deceleration for " + std::string(joint_name) + " to at least " +
         planning + " rad/s^2";
}

} // namespace manyforge::deployment::detail
