#pragma once

#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>

#include <manyforge_core/types/collision.hpp>
#include <manyforge_core/types/robot_defaults.generated.hpp>

namespace manyforge::core {

/// Planner block: the Deployment's planner
/// default per robot, optionally overridden per motion (the node's `planner`
/// parameter). Joint and pose goals use it; Cartesian paths do not search.

/// `search`: how the tree grows. RRT-Connect grows two trees and connects them.
enum class PlannerSearchDTO {
  kRrt,
  kRrtConnect,
};

/// `optimize`: first = return the first path; rrt_star = rewire and spend the
/// node/time budget on the cheapest path; best_of_n = plan `n` times with
/// distinct seeds and keep the shortest path.
enum class PlannerOptimizeDTO {
  kFirst,
  kRrtStar,
  kBestOfN,
};

[[nodiscard]] inline constexpr std::optional<PlannerSearchDTO> planner_search_from_string(
    std::string_view value) noexcept {
  if (value == "rrt") return PlannerSearchDTO::kRrt;
  if (value == "rrt_connect") return PlannerSearchDTO::kRrtConnect;
  return std::nullopt;
}

[[nodiscard]] inline constexpr std::optional<PlannerOptimizeDTO> planner_optimize_from_string(
    std::string_view value) noexcept {
  if (value == "first") return PlannerOptimizeDTO::kFirst;
  if (value == "rrt_star") return PlannerOptimizeDTO::kRrtStar;
  if (value == "best_of_n") return PlannerOptimizeDTO::kBestOfN;
  return std::nullopt;
}

/// The Deployment planner default (manyforge_core/resources/robot_defaults.tsv
/// holds it and kDefaultPlannerBestOfN, kDefaultPlannerMaxNodes).
inline constexpr PlannerSearchDTO kDefaultPlannerSearch =
    planner_search_from_string(kDefaultPlannerSearchName).value();
inline constexpr PlannerOptimizeDTO kDefaultPlannerOptimize =
    planner_optimize_from_string(kDefaultPlannerOptimizeName).value();
inline constexpr std::uint32_t kMinPlannerBestOfN = 2U;
inline constexpr std::uint32_t kMaxPlannerBestOfN = 32U;
inline constexpr std::uint32_t kMinPlannerMaxNodes = 100U;
inline constexpr std::uint32_t kMaxPlannerMaxNodes = 1'000'000U;
inline constexpr double kMaxCollisionCheckStepRad = 0.2;

/// One motion's planner override; each member left unset takes the
/// Deployment planner default.
struct PlannerOverrideDTO {
  std::optional<PlannerSearchDTO> search;
  std::optional<PlannerOptimizeDTO> optimize;
  std::optional<std::uint32_t> n;
  std::optional<double> collision_check_step_rad;
  std::optional<std::uint32_t> max_nodes;
  friend bool operator==(const PlannerOverrideDTO&, const PlannerOverrideDTO&) = default;
};

/// The effective planner settings of one plan.
struct PlannerSettingsDTO {
  PlannerSearchDTO search{kDefaultPlannerSearch};
  PlannerOptimizeDTO optimize{kDefaultPlannerOptimize};
  std::uint32_t n{kDefaultPlannerBestOfN};
  double collision_check_step_rad{kDefaultCollisionCheckStepRad};
  std::uint32_t max_nodes{kDefaultPlannerMaxNodes};
  friend bool operator==(const PlannerSettingsDTO&, const PlannerSettingsDTO&) = default;
};

[[nodiscard]] inline PlannerSettingsDTO resolve_planner_settings(
    PlannerSettingsDTO defaults, const PlannerOverrideDTO& override_value) {
  if (override_value.search) defaults.search = *override_value.search;
  if (override_value.optimize) defaults.optimize = *override_value.optimize;
  if (override_value.n) defaults.n = *override_value.n;
  if (override_value.collision_check_step_rad)
    defaults.collision_check_step_rad = *override_value.collision_check_step_rad;
  if (override_value.max_nodes) defaults.max_nodes = *override_value.max_nodes;
  return defaults;
}

[[nodiscard]] inline bool valid_planner_best_of_n(std::uint32_t n) noexcept {
  return n >= kMinPlannerBestOfN && n <= kMaxPlannerBestOfN;
}
[[nodiscard]] inline bool valid_planner_max_nodes(std::uint32_t max_nodes) noexcept {
  return max_nodes >= kMinPlannerMaxNodes && max_nodes <= kMaxPlannerMaxNodes;
}
[[nodiscard]] inline bool valid_collision_check_step_rad(double step) noexcept {
  return std::isfinite(step) && step > 0.0 && step <= kMaxCollisionCheckStepRad;
}

[[nodiscard]] inline bool valid_planner_settings(const PlannerSettingsDTO& value) noexcept {
  return valid_planner_best_of_n(value.n) && valid_planner_max_nodes(value.max_nodes) &&
         valid_collision_check_step_rad(value.collision_check_step_rad);
}

[[nodiscard]] inline bool valid_planner_override(const PlannerOverrideDTO& value) noexcept {
  return (!value.n || valid_planner_best_of_n(*value.n)) &&
         (!value.max_nodes || valid_planner_max_nodes(*value.max_nodes)) &&
         (!value.collision_check_step_rad ||
          valid_collision_check_step_rad(*value.collision_check_step_rad));
}

[[nodiscard]] inline constexpr std::string_view to_string(PlannerSearchDTO value) noexcept {
  return value == PlannerSearchDTO::kRrt ? "rrt" : "rrt_connect";
}

[[nodiscard]] inline constexpr std::string_view to_string(PlannerOptimizeDTO value) noexcept {
  switch (value) {
    case PlannerOptimizeDTO::kFirst:
      return "first";
    case PlannerOptimizeDTO::kRrtStar:
      return "rrt_star";
    case PlannerOptimizeDTO::kBestOfN:
      return "best_of_n";
  }
  return "first";
}

}  // namespace manyforge::core
