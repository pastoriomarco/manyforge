#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "direct_control_manual_runs.hpp"

namespace manyforge::roboplan_backend::host_detail {
namespace {

struct FakeTargetBinding final {
  std::string controllable_component_id;
  std::string command_target_id;
};

struct FakeProduct final {
  FakeTargetBinding binding;
  [[nodiscard]] const FakeTargetBinding &value() const { return binding; }
};

struct FakeComposition final {
  std::optional<FakeProduct> direct_control_motion_binding;
  std::optional<FakeProduct> direct_control_gripper_binding;
  std::vector<std::unique_ptr<FakeComposition>> participant_compositions;
};

struct FakeTarget final {
  std::string component_id;
  std::string command_target_id;
};

// The kernel outlives an Apply: it keeps the latest manual operation of every
// target of the previous cell, including a removed gripper.
struct FakeKernel final {
  std::map<std::pair<std::string, std::string>, std::string> latest{
      {{"robot-a", "arm"}, "arm-a-op"},
      {{"robot-a", "gripper"}, "gripper-a-op"},
      {{"robot-b", "arm"}, "arm-b-op"}};

  [[nodiscard]] std::vector<std::string>
  get_latest_manual_operations(const std::vector<FakeTarget> &targets) const {
    std::vector<std::string> result;
    for (const auto &target : targets) {
      const auto found = latest.find({target.component_id, target.command_target_id});
      if (found != latest.end()) result.push_back(found->second);
    }
    return result;
  }
};

FakeProduct Target(std::string component, std::string command_target) {
  return FakeProduct{{std::move(component), std::move(command_target)}};
}

std::vector<std::string> Published(const FakeComposition &composition) {
  return CurrentDirectControlManualOperations<FakeTarget>(composition, FakeKernel{});
}

TEST(DirectControlManualRuns, RobotAndGripperPublishBoth) {
  FakeComposition cell{Target("robot-a", "arm"), Target("robot-a", "gripper"), {}};
  EXPECT_EQ(Published(cell), (std::vector<std::string>{"arm-a-op", "gripper-a-op"}));
}

TEST(DirectControlManualRuns, RobotOnlyAfterGripperRemovalPublishesOnlyMotion) {
  FakeComposition cell{Target("robot-a", "arm"), std::nullopt, {}};
  EXPECT_EQ(Published(cell), (std::vector<std::string>{"arm-a-op"}));
}

TEST(DirectControlManualRuns, EmptyCellPublishesNone) {
  EXPECT_TRUE(Published(FakeComposition{}).empty());
}

TEST(DirectControlManualRuns, ParticipantGripperCountsAsCurrent) {
  FakeComposition cell{Target("robot-a", "arm"), std::nullopt, {}};
  cell.participant_compositions.push_back(std::make_unique<FakeComposition>(
      FakeComposition{std::nullopt, Target("robot-a", "gripper"), {}}));
  EXPECT_EQ(Published(cell), (std::vector<std::string>{"arm-a-op", "gripper-a-op"}));
}

TEST(DirectControlManualRuns, TwoArmsInOneCellEachKeepTheirOwnResult) {
  FakeComposition cell{Target("robot-a", "arm"), std::nullopt, {}};
  cell.participant_compositions.push_back(std::make_unique<FakeComposition>(
      FakeComposition{Target("robot-b", "arm"), std::nullopt, {}}));
  EXPECT_EQ(Published(cell), (std::vector<std::string>{"arm-a-op", "arm-b-op"}));
}

}  // namespace
}  // namespace manyforge::roboplan_backend::host_detail
