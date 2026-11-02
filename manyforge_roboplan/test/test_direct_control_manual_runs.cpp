#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "direct_control_manual_runs.hpp"

namespace manyforge::roboplan_backend::host_detail {
namespace {

struct FakeComposition final {
  std::optional<int> direct_control_motion_binding;
  std::optional<int> direct_control_gripper_binding;
  std::vector<std::unique_ptr<FakeComposition>> participant_compositions;
};

// The kernel outlives an Apply: it keeps the latest operation of both
// direct-control runs from the previous robot+gripper cell.
struct FakeKernel final {
  std::map<std::string, std::string> latest{
      {"direct-control", "arm-op"}, {"direct-control-gripper", "gripper-op"}};

  [[nodiscard]] std::vector<std::string>
  get_latest_run_operations(const std::vector<std::string> &run_ids) const {
    std::vector<std::string> result;
    for (const auto &run : run_ids) result.push_back(latest.at(run));
    return result;
  }
};

std::vector<std::string> Published(const FakeComposition &composition) {
  return CurrentDirectControlManualOperations(
      composition, FakeKernel{}, std::string("direct-control"),
      std::string("direct-control-gripper"));
}

TEST(DirectControlManualRuns, RobotAndGripperPublishBoth) {
  FakeComposition cell{1, 2, {}};
  EXPECT_EQ(Published(cell), (std::vector<std::string>{"arm-op", "gripper-op"}));
}

TEST(DirectControlManualRuns, RobotOnlyAfterGripperRemovalPublishesOnlyMotion) {
  FakeComposition cell{1, std::nullopt, {}};
  EXPECT_EQ(Published(cell), (std::vector<std::string>{"arm-op"}));
}

TEST(DirectControlManualRuns, EmptyCellPublishesNone) {
  EXPECT_TRUE(Published(FakeComposition{}).empty());
}

TEST(DirectControlManualRuns, ParticipantGripperCountsAsCurrent) {
  FakeComposition cell{1, std::nullopt, {}};
  cell.participant_compositions.push_back(
      std::make_unique<FakeComposition>(FakeComposition{std::nullopt, 2, {}}));
  EXPECT_EQ(Published(cell), (std::vector<std::string>{"arm-op", "gripper-op"}));
}

}  // namespace
}  // namespace manyforge::roboplan_backend::host_detail
