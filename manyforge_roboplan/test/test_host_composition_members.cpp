#include "ros_host_composition.hpp"

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace manyforge::roboplan_backend::host_detail {
namespace {

std::unique_ptr<Composition> View(const std::string &component) {
  return std::make_unique<Composition>(
      core::ControllableComponentId(component), nullptr, nullptr, nullptr, nullptr, nullptr,
      std::nullopt, std::nullopt, std::nullopt, CanonicalValue(nullptr), CanonicalValue(nullptr),
      nullptr, nullptr, core::ProjectionRevisionDTO{}, core::ActiveBundlePublicationGeneration(1U),
      std::nullopt, std::nullopt);
}

// An armed machine's owner has no view of its own; it
// rides on its arm member's view, which retains it on a retained Apply.
TEST(MemberCarriedOwnerIdsTest, NamesOwnersCarriedByAnArmMemberOnly) {
  auto cell = View("uf850_robot");
  cell->owner_component = core::CoordinatedComponent{"uf850", nullptr};
  cell->owner_id = "uf850";
  auto io = View("uf850_robot_tool_io");
  io->owner_id = "uf850";  // an I/O member names its owner, carries none
  cell->participant_compositions.push_back(std::move(io));
  cell->participant_compositions.push_back(View("xarm_gripper"));
  EXPECT_EQ(MemberCarriedOwnerIds(*cell),
            (std::vector{core::ControllableComponentId("uf850")}));
}

TEST(MemberCarriedOwnerIdsTest, AnArmlessOwnerWithItsOwnViewIsNotCarried) {
  auto owner = View("lite6");
  owner->owner_component = core::CoordinatedComponent{"lite6", nullptr};
  auto io = View("lite6_tool_io");
  io->owner_id = "lite6";
  owner->participant_compositions.push_back(std::move(io));
  EXPECT_TRUE(MemberCarriedOwnerIds(*owner).empty());
}

// One qualified physical source of the cell: a component's publication of
// its model coordinates, with or without a coordinate observation.
std::shared_ptr<core::ObservedRobotStatePublication> Publication(
    std::vector<std::string> joints) {
  return std::make_shared<core::ObservedRobotStatePublication>(
      std::move(joints),
      std::vector<core::ObservedRobotStateSourceConfig>{core::ObservedRobotStateSourceConfig{
          core::ObservedRobotStateSourceKind::kCommandedState, "commanded",
          core::ObservedRobotStateAuthorityProfile::kCommandedSimulation, 0}});
}

CellFrameStateSource FrameSource(
    const std::string &component,
    std::shared_ptr<const core::ObservedRobotStatePublication> publication,
    const RetainedSceneFrameCoordinateAuthority authority) {
  core::ObservedStateQueryContextDTO context{
      core::ControllableComponentId(component),
      core::CommandTargetId("target"),
      core::CapabilityId("state.observed.current"),
      core::ControlDomainId("domain"),
      core::ControlDomainRevision("domain-revision"),
      core::ControlBindingId("binding"),
      core::ControlBindingRevision("binding-revision"),
      core::ControllerTopologyId("topology"),
      core::TopologyRevision("topology-revision"),
      core::ControlRouteId("route"),
      {},
      {},
      {},
      core::ControlInterfaceContractId("interface"),
      core::ControlInterfaceRevision("interface-revision"),
      core::ActiveControlConfigurationId("active"),
      core::ActiveControlConfigurationGeneration(1U),
      core::ProviderInstanceId("provider"),
      core::ProviderManifestId("manifest"),
      core::ProviderManifestRevision("manifest-revision"),
      core::RuntimeEpoch("runtime-epoch"),
      core::StateAuthorityId("authority"),
      core::StateAuthorityClass::kMeasured,
      0};
  RetainedSceneFrameStateSourceInput frame{
      std::move(context), core::ObservedStateSourceKind::kPredictedState, nullptr,
      authority};
  return {std::move(frame), std::move(publication)};
}

// A pneumatic gripper has finger joints in the cell model and no coordinate
// observation. It is not a source: its coordinates keep the model's value,
// and the robots of the cell plan and are checked as if it published nothing.
TEST(CellModelStateSourcesOfTest, AComponentWithoutACoordinateObservationIsNotASource) {
  const auto robot_b_state = Publication({"b1"});
  const auto robot_a = FrameSource("robot_a", Publication({"a1"}),
                                   RetainedSceneFrameCoordinateAuthority::kRequiredPublication);
  const auto robot_b = FrameSource("robot_b", robot_b_state,
                                   RetainedSceneFrameCoordinateAuthority::kRequiredPublication);
  const auto pneumatic = FrameSource(
      "pneumatic_gripper", Publication({"finger"}),
      RetainedSceneFrameCoordinateAuthority::kUnavailablePneumatic);
  const std::vector<CellFrameStateSource> collected{robot_a, pneumatic, robot_b};
  std::map<std::string, std::shared_ptr<core::ComponentStreamStaleFlag>> flags;
  const auto cell = CellModelStateSourcesOf(
      collected, [&](const core::ControllableComponentId &component) {
        return flags[component.value()] = std::make_shared<core::ComponentStreamStaleFlag>();
      });
  ASSERT_NE(cell, nullptr);
  ASSERT_EQ(cell->size(), 2U);
  EXPECT_EQ((*cell)[0].component_id.value(), "robot_a");
  EXPECT_EQ((*cell)[0].publication, robot_a.numeric_publication);
  EXPECT_EQ((*cell)[0].stream_stale, flags.at("robot_a"));
  EXPECT_EQ((*cell)[1].component_id.value(), "robot_b");
  EXPECT_EQ((*cell)[1].stream_stale, flags.at("robot_b"));
  EXPECT_FALSE(flags.contains("pneumatic_gripper"));

  // robot_a's view of the cell: robot_b where it is observed, the gripper's
  // finger at the model's value, and nothing absent although the gripper
  // never published.
  const std::vector<std::string> joint_names{"b1"};
  const std::vector<double> positions{0.6};
  ASSERT_TRUE(robot_b_state
                  ->publish(core::ObservedRobotStateSourceKind::kCommandedState, joint_names,
                            positions, {}, 10, 10)
                  .published);
  const std::vector<std::string> model{"a1", "finger", "b1"};
  const std::vector<std::string> own{"a1"};
  const auto bound = core::bind_cell_model_state(*cell, model, own);
  ASSERT_EQ(bound.size(), 1U);
  std::vector<double> coordinates{0.1, 0.003, 0.0};
  EXPECT_FALSE(static_cast<bool>(
      core::overlay_cell_model_state(bound, 20, std::span<double>(coordinates))));
  EXPECT_EQ(coordinates, (std::vector<double>{0.1, 0.003, 0.6}));
}

// The installed composition's sources are what every robot's Kernel and
// planner read through the host's stable handle, from the moment the
// composition is selected.
TEST(ExecutionCompositionAccessTest, ReplaceCurrentInstallsTheCellsStateSources) {
  ExecutionCompositionAccess access;
  ASSERT_NE(access.cell_model_state, nullptr);
  EXPECT_EQ(access.cell_model_state->Get(), nullptr);

  auto first = View("robot_a");
  first->cell_model_state_sources = std::make_shared<const core::CellModelStateSources>();
  auto second = View("robot_a");
  second->cell_model_state_sources = std::make_shared<const core::CellModelStateSources>();
  // What the first reader of a newly selected composition sees.
  std::shared_ptr<const core::CellModelStateSources> seen_at_selection;
  access.publish_transport_membership = [&](const Composition *selected) {
    EXPECT_EQ(access.current, selected);
    seen_at_selection = access.cell_model_state->Get();
  };
  {
    std::unique_lock lock(access.mutex);
    const auto removed = access.replace_current(first.get());
    EXPECT_TRUE(removed.empty());
  }
  EXPECT_EQ(access.cell_model_state->Get(), first->cell_model_state_sources);
  EXPECT_EQ(seen_at_selection, first->cell_model_state_sources);
  {
    std::unique_lock lock(access.mutex);
    const auto removed = access.replace_current(second.get());
    EXPECT_TRUE(removed.empty());
  }
  EXPECT_EQ(access.cell_model_state->Get(), second->cell_model_state_sources);
  EXPECT_EQ(seen_at_selection, second->cell_model_state_sources);
  {
    std::unique_lock lock(access.mutex);
    const auto removed = access.replace_current(nullptr);
    EXPECT_TRUE(removed.empty());
  }
  EXPECT_EQ(access.cell_model_state->Get(), nullptr);
}

}  // namespace
}  // namespace manyforge::roboplan_backend::host_detail
