#include <gtest/gtest.h>

#include <manyforge_core/cell_model_state.hpp>
#include <manyforge_core/component_stream_health.hpp>

#include <memory>
#include <string>
#include <vector>

namespace manyforge::core {
namespace {

// A measured joint-state source with its stream limit, or (limit 0) a
// commanded state, which does not expire.
ObservedRobotStateSourceKind source_kind(std::int64_t stream_limit_ns) {
  return stream_limit_ns > 0 ? ObservedRobotStateSourceKind::kJointState
                             : ObservedRobotStateSourceKind::kCommandedState;
}

std::shared_ptr<ObservedRobotStatePublication> publication(std::vector<std::string> joints,
                                                           std::int64_t stream_limit_ns) {
  return std::make_shared<ObservedRobotStatePublication>(
      std::move(joints),
      std::vector<ObservedRobotStateSourceConfig>{ObservedRobotStateSourceConfig{
          source_kind(stream_limit_ns), "source",
          stream_limit_ns > 0 ? ObservedRobotStateAuthorityProfile::kMeasured
                              : ObservedRobotStateAuthorityProfile::kCommandedSimulation,
          stream_limit_ns}});
}

void publish(ObservedRobotStatePublication& target, std::int64_t stream_limit_ns,
             const std::vector<double>& positions, std::int64_t received_ns) {
  const std::vector<std::string> names(target.canonical_joint_names().begin(),
                                       target.canonical_joint_names().end());
  ASSERT_TRUE(target
                  .publish(source_kind(stream_limit_ns), names, positions, {}, received_ns,
                           received_ns)
                  .published);
}

TEST(CellModelStateTest, BindingLeavesOutTheCallersOwnSourceAndUnknownCoordinates) {
  const auto robot_a = publication({"a1", "a2"}, 0);
  const auto robot_b = publication({"b1", "b2"}, 0);
  const auto gripper = publication({"finger", "local_only"}, 0);
  const auto elsewhere = publication({"not_in_model"}, 0);
  const CellModelStateSources cell{{ControllableComponentId("robot_a"), robot_a},
                                   {ControllableComponentId("robot_b"), robot_b},
                                   {ControllableComponentId("gripper"), gripper},
                                   {ControllableComponentId("elsewhere"), elsewhere}};
  const std::vector<std::string> model{"a1", "a2", "finger", "b1", "b2", "passive"};
  const std::vector<std::string> own{"a1", "a2"};

  const auto bound = bind_cell_model_state(cell, model, own);
  ASSERT_EQ(bound.size(), 2U);
  EXPECT_EQ(bound[0].component_id.value(), "robot_b");
  EXPECT_EQ(bound[0].target_indices, (std::vector<std::size_t>{3U, 4U}));
  EXPECT_EQ(bound[1].component_id.value(), "gripper");
  EXPECT_EQ(bound[1].target_indices,
            (std::vector<std::size_t>{2U, BoundCellModelStateSource::kNotBound}));
  EXPECT_FALSE(cell_model_state_binds(bound, 0U));
  EXPECT_TRUE(cell_model_state_binds(bound, 2U));
  EXPECT_TRUE(cell_model_state_binds(bound, 4U));
  EXPECT_FALSE(cell_model_state_binds(bound, 5U));
}

TEST(CellModelStateTest, OverlayPlacesTheOtherComponentsInEveryWaypoint) {
  const auto robot_b = publication({"b1", "b2"}, 0);
  const auto gripper = publication({"finger"}, 0);
  const CellModelStateSources cell{{ControllableComponentId("robot_b"), robot_b},
                                   {ControllableComponentId("gripper"), gripper}};
  TrajectoryDTO trajectory{{},
                           {"a1", "finger", "b1", "b2", "passive"},
                           {{0.0, {0.1, 0.0, 0.0, 0.0, 0.9}, {}, {}},
                            {1.0, {0.2, 0.0, 0.0, 0.0, 0.9}, {}, {}},
                            {2.0, {0.3, 0.0, 0.0, 0.0, 0.9}, {}, {}}}};
  const std::vector<std::string> own{"a1"};
  const auto bound = bind_cell_model_state(cell, trajectory.joint_names, own);
  publish(*robot_b, 0, {1.5, -0.5}, 10);
  publish(*gripper, 0, {0.004}, 10);

  EXPECT_FALSE(static_cast<bool>(overlay_cell_model_state(bound, 20, trajectory)));
  const std::vector<double> own_positions{0.1, 0.2, 0.3};
  for (std::size_t waypoint = 0; waypoint < trajectory.waypoints.size(); ++waypoint) {
    EXPECT_EQ(trajectory.waypoints[waypoint].positions_rad,
              (std::vector<double>{own_positions[waypoint], 0.004, 1.5, -0.5, 0.9}));
  }
}

TEST(CellModelStateTest, AStateIsStaleExactlyWhileItsComponentIsStreamStale) {
  // Both publications carry a lane budget; neither decides staleness. Only
  // the component's stream-stale flag does, whatever the sample's age.
  constexpr std::int64_t kLaneBudgetNs = 250;
  const auto measured = publication({"b1"}, kLaneBudgetNs);
  const auto commanded = publication({"finger"}, 0);
  const auto robot_b_stream = std::make_shared<ComponentStreamStaleFlag>();
  const CellModelStateSources cell{
      {ControllableComponentId("gripper"), commanded},
      {ControllableComponentId("robot_b"), measured, robot_b_stream}};
  const std::vector<std::string> model{"a1", "finger", "b1"};
  const std::vector<std::string> own{"a1"};
  const auto bound = bind_cell_model_state(cell, model, own);
  ASSERT_EQ(bound.size(), 2U);
  std::vector<double> coordinates{0.0, 0.0, 0.0};

  // robot_b has published nothing.
  publish(*commanded, 0, {0.004}, 10);
  auto unavailable = overlay_cell_model_state(bound, 20, std::span<double>(coordinates));
  ASSERT_TRUE(static_cast<bool>(unavailable));
  EXPECT_EQ(unavailable.verdict, PeerStateVerdict::kAbsent);
  EXPECT_EQ(unavailable.source->component_id.value(), "robot_b");
  // The refusing path keeps its own code; the condition is the reason.
  auto error = peer_state_unavailable_error(unavailable, "planning_failed");
  EXPECT_EQ(error.code, "planning_failed");
  EXPECT_EQ(error.details.at("reason"), kComponentStreamAbsentCode);
  EXPECT_EQ(error.details.at("peer_component_id"), "robot_b");

  publish(*measured, kLaneBudgetNs, {0.7}, 1000);
  // Far older than the lane budget, and the component's stream is live: used.
  EXPECT_FALSE(static_cast<bool>(overlay_cell_model_state(
      bound, 1000 + 100 * kLaneBudgetNs, std::span<double>(coordinates))));
  EXPECT_EQ(coordinates, (std::vector<double>{0.0, 0.004, 0.7}));

  // A fresh sample, and the component is stream-stale: not used.
  robot_b_stream->value.store(true);
  unavailable = overlay_cell_model_state(bound, 1000, std::span<double>(coordinates));
  ASSERT_TRUE(static_cast<bool>(unavailable));
  EXPECT_EQ(unavailable.verdict, PeerStateVerdict::kStale);
  EXPECT_EQ(unavailable.source->component_id.value(), "robot_b");
  error = peer_state_unavailable_error(unavailable, "validation_collision");
  EXPECT_EQ(error.code, "validation_collision");
  EXPECT_EQ(error.details.at("reason"), kComponentStreamStaleCode);
  EXPECT_EQ(error.details.at("peer_component_id"), "robot_b");

  // Live again: used again.
  robot_b_stream->value.store(false);
  EXPECT_FALSE(static_cast<bool>(
      overlay_cell_model_state(bound, 1000, std::span<double>(coordinates))));
}

}  // namespace
}  // namespace manyforge::core
