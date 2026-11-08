#include <gtest/gtest.h>

#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_core/tcp_selector_codec.hpp>
#include <manyforge_core/active_tcp_blackboard_key.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <string>
#include <vector>

namespace {

using manyforge::content_identity::CanonicalValue;
using manyforge::content_identity::canonical_json;
using namespace manyforge::core;

[[nodiscard]] TcpEntryDTO gripper_tcp() {
  // Ids may contain dots; the entry is never joined into one string.
  return TcpEntryDTO{ControllableComponentId("cell.lite_gripper"), CommandTargetId("gripper.main"),
                     FrameRoleId("tcp")};
}

[[nodiscard]] TcpEntryReferenceDTO mounted_tool_reference() {
  return {{ControllableComponentId("cell.robot"), CommandTargetId("arm")},
          {ControllableComponentId("cell.lite_gripper"), FrameRoleId("tcp")}};
}

TEST(TcpSelectorCodecTest, TheThreeFormsRoundTrip) {
  const std::vector<TcpSelectorDTO> forms{CurrentTcpDTO{},
                                          DefaultTcpOfTargetDTO{{ControllableComponentId("cell.robot"), CommandTargetId("arm")}},
                                          mounted_tool_reference()};
  for (const auto& form : forms) {
    const auto encoded = EncodeTcpSelector(form);
    const auto decoded = DecodeTcpSelector(encoded);
    ASSERT_TRUE(decoded.ok()) << decoded.error().message;
    EXPECT_EQ(decoded.value(), form) << canonical_json(encoded);
    EXPECT_EQ(canonical_json(EncodeTcpSelector(decoded.value())), canonical_json(encoded));
  }
}

TEST(TcpSelectorCodecTest, WireShapesAreClosedAndStructured) {
  EXPECT_EQ(canonical_json(EncodeTcpSelector(CurrentTcpDTO{})), R"({"kind":"current"})");
  EXPECT_EQ(canonical_json(EncodeTcpSelector(DefaultTcpOfTargetDTO{
                {ControllableComponentId("cell.robot"), CommandTargetId("arm")}})),
            R"({"kind":"default_of_target","target":{"command_target_id":"arm","component_id":"cell.robot"}})");
  EXPECT_EQ(canonical_json(EncodeTcpSelector(mounted_tool_reference())),
            R"({"frame":{"component_id":"cell.lite_gripper","frame_id":"tcp"},)"
            R"("kind":"entry","target":{"command_target_id":"arm","component_id":"cell.robot"}})");
  const auto direct = DecodeTcpEntryReference(EncodeTcpEntryReference(mounted_tool_reference()));
  ASSERT_TRUE(direct.ok());
  EXPECT_EQ(direct.value(), mounted_tool_reference());
  const auto entry = DecodeTcpEntry(EncodeTcpEntry(gripper_tcp()));
  ASSERT_TRUE(entry.ok());
  EXPECT_EQ(entry.value(), gripper_tcp());
}

TEST(TcpSelectorCodecTest, MountedToolFrameKeepsItsSeparateOwner) {
  const auto ref = mounted_tool_reference();
  const RuntimeMotionEndpointBindingDTO endpoint{
      ref.target, {ref.frame, ModelInstanceId("tool.model"), ModelRevision("r1"), "tool/tcp"}, {}};
  EXPECT_TRUE(tcp_selector_admits(TcpSelectorDTO{ref}, endpoint,
                                  ControllableComponentId("cell.lite_gripper")));
  EXPECT_FALSE(tcp_selector_admits(TcpSelectorDTO{ref}, endpoint,
                                   ControllableComponentId("cell.other_tool")));
  auto wrong_target = ref;
  wrong_target.target.component_id = ControllableComponentId("cell.other_robot");
  EXPECT_FALSE(tcp_selector_admits(TcpSelectorDTO{wrong_target}, endpoint,
                                   ControllableComponentId("cell.lite_gripper")));
}

TEST(TcpSelectorCodecTest, ReservedActiveTcpKeyUsesExactlyThreeSegments) {
  const ProgramTargetReferenceDTO target{ControllableComponentId("lite6_robot"),
                                         CommandTargetId("2_ARM")};
  const auto key = ActiveTcpBlackboardKey(target);
  ASSERT_TRUE(key.has_value());
  EXPECT_EQ(*key, "active_tcp/lite6_robot/2_ARM");
  EXPECT_EQ(ParseActiveTcpBlackboardKey(*key), target);
  EXPECT_FALSE(ParseActiveTcpBlackboardKey("active_tcp/lite6_robot/arm/extra"));
  EXPECT_FALSE(ParseActiveTcpBlackboardKey("active_tcp//arm"));
  EXPECT_FALSE(ParseActiveTcpBlackboardKey("active_tcp/lite6_robot/"));
  EXPECT_FALSE(ParseActiveTcpBlackboardKey("ordinary/lite6_robot/arm"));
}

TEST(TcpSelectorCodecTest, ThePreM7EndpointIsRefusedTyped) {
  const CanonicalValue legacy(CanonicalValue::Object{
      {"target_role_id", CanonicalValue("arm")}, {"frame_role_id", CanonicalValue("tcp")}});
  const auto refused = DecodeTcpSelector(legacy);
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().code, "invalid_request");
  EXPECT_EQ(refused.error().details.at("reason"), "tcp_endpoint_legacy_form");
}

TEST(TcpSelectorCodecTest, MalformedSelectorsAreRefused) {
  using Object = CanonicalValue::Object;
  const std::vector<CanonicalValue> malformed{
      CanonicalValue("current"),
      CanonicalValue(Object{{"kind", CanonicalValue("latest")}}),
      CanonicalValue(Object{{"kind", CanonicalValue("current")}, {"role", CanonicalValue("x")}}),
      CanonicalValue(Object{{"kind", CanonicalValue("default_of_role")}}),
      CanonicalValue(Object{{"kind", CanonicalValue("default_of_role")}, {"role", CanonicalValue("")}}),
      CanonicalValue(Object{{"kind", CanonicalValue("entry")}}),
      CanonicalValue(Object{{"kind", CanonicalValue("entry")},
                            {"entry", CanonicalValue(Object{{"component_id", CanonicalValue("c")},
                                                            {"target_id", CanonicalValue("t")}})}}),
      CanonicalValue(Object{
          {"kind", CanonicalValue("entry")},
          {"entry", CanonicalValue(Object{{"component_id", CanonicalValue("c")},
                                          {"target_id", CanonicalValue("t")},
                                          {"frame_role_id", CanonicalValue("")}})}})};
  for (const auto& value : malformed) {
    const auto refused = DecodeTcpSelector(value);
    ASSERT_FALSE(refused.ok()) << canonical_json(value);
    EXPECT_EQ(refused.error().details.at("reason"), "tcp_selector_invalid") << canonical_json(value);
  }
}

TEST(TcpSelectorCodecTest, TcpRefusalsAreTypedAndNameTheRobot) {
  const auto robot = ControllableComponentId("lite6_robot");
  const auto not_mounted = tcp_tool_not_mounted_error(robot, gripper_tcp());
  EXPECT_EQ(not_mounted.code, "execution_rejected");
  EXPECT_EQ(not_mounted.details.at("reason"), "tcp_tool_not_mounted");
  EXPECT_EQ(not_mounted.details.at("robot"), "lite6_robot");
  EXPECT_EQ(not_mounted.details.at("tool"), "cell.lite_gripper");
  const auto unknown = active_tcp_unknown_error(robot, gripper_tcp());
  EXPECT_EQ(unknown.details.at("reason"), "active_tcp_unknown");
  EXPECT_EQ(unknown.details.at("target_id"), "gripper.main");
  const auto direct_unknown = active_tcp_unknown_error(robot, mounted_tool_reference());
  EXPECT_EQ(direct_unknown.details.at("component_id"), "cell.robot");
  EXPECT_EQ(direct_unknown.details.at("command_target_id"), "arm");
  EXPECT_EQ(direct_unknown.details.at("frame_component_id"), "cell.lite_gripper");
  EXPECT_EQ(direct_unknown.details.at("frame_id"), "tcp");
  EXPECT_EQ(tcp_tool_not_mounted_error(robot, mounted_tool_reference()).details.at("tool"),
            "cell.lite_gripper");
}

}  // namespace
