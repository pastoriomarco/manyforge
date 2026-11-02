#include <gtest/gtest.h>

#include <array>
#include <vector>

#include <manyforge_core/types/tool_context.hpp>

namespace manyforge::core {
namespace {

Pose3DTO Offset(double z) { return {{0.0, 0.0, z}, {0.0, 0.0, 0.0, 1.0}}; }

// TOOLCTX: the payload is the sum of the masses at their mass-weighted mean.
TEST(ToolContext, PayloadIsTheMassSumAtTheMassWeightedCentre) {
  // A 0.5 kg tool with its centre 40 mm out, a 0.25 kg object at the TCP
  // (100 mm out, 20 mm to the side).
  const std::vector<ToolContextPointMassDTO> masses{
      {0.5, {0.0, 0.0, 0.04}}, {0.25, {0.02, 0.0, 0.1}}};
  const auto context = compose_tool_context(Offset(0.1), masses);
  EXPECT_EQ(context.tcp_pose_in_flange, Offset(0.1));
  EXPECT_DOUBLE_EQ(context.payload_mass_kg, 0.75);
  EXPECT_DOUBLE_EQ(context.payload_center_of_mass_m[0], 0.25 * 0.02 / 0.75);
  EXPECT_DOUBLE_EQ(context.payload_center_of_mass_m[1], 0.0);
  EXPECT_DOUBLE_EQ(context.payload_center_of_mass_m[2],
                   (0.5 * 0.04 + 0.25 * 0.1) / 0.75);
}

TEST(ToolContext, NoMassIsAZeroPayloadAtTheFlange) {
  const std::vector<ToolContextPointMassDTO> massless{{0.0, {0.3, 0.3, 0.3}}};
  for (const auto& masses : {std::vector<ToolContextPointMassDTO>{}, massless}) {
    const auto context = compose_tool_context(Offset(0.0), masses);
    EXPECT_EQ(context.payload_mass_kg, 0.0);
    EXPECT_EQ(context.payload_center_of_mass_m, (std::array<double, 3>{0.0, 0.0, 0.0}));
  }
}

TEST(ToolContext, AttachAndDetachChangeOnlyThePayload) {
  const std::vector<ToolContextPointMassDTO> tool{{0.5, {0.0, 0.0, 0.04}}};
  auto attached = tool;
  attached.push_back({1.0, {0.0, 0.0, 0.1}});
  const auto empty = compose_tool_context(Offset(0.1), tool);
  const auto carrying = compose_tool_context(Offset(0.1), attached);
  EXPECT_EQ(carrying.tcp_pose_in_flange, empty.tcp_pose_in_flange);
  EXPECT_DOUBLE_EQ(carrying.payload_mass_kg, 1.5);
  EXPECT_DOUBLE_EQ(carrying.payload_center_of_mass_m[2], (0.02 + 0.1) / 1.5);
  // Detached again: exactly the empty context, so nothing is resent.
  EXPECT_EQ(compose_tool_context(Offset(0.1), tool), empty);
  // A TCP switch changes only the TCP.
  const auto switched = compose_tool_context(Offset(0.0), tool);
  EXPECT_NE(switched.tcp_pose_in_flange, empty.tcp_pose_in_flange);
  EXPECT_EQ(switched.payload_mass_kg, empty.payload_mass_kg);
  EXPECT_EQ(switched.payload_center_of_mass_m, empty.payload_center_of_mass_m);
}

}  // namespace
}  // namespace manyforge::core
