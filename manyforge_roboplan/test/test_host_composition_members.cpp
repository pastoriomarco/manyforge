#include "ros_host_composition.hpp"

#include <gtest/gtest.h>

#include <memory>
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

// H1 (night 2026-11-02): an armed machine's owner has no view of its own; it
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

}  // namespace
}  // namespace manyforge::roboplan_backend::host_detail
