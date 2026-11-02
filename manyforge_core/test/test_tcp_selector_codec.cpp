#include <gtest/gtest.h>

#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_core/tcp_selector_codec.hpp>
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

TEST(TcpSelectorCodecTest, TheThreeFormsRoundTrip) {
  const std::vector<TcpSelectorDTO> forms{CurrentTcpDTO{},
                                          DefaultTcpOfRoleDTO{TargetRoleId("gripper")},
                                          gripper_tcp()};
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
  EXPECT_EQ(canonical_json(EncodeTcpSelector(DefaultTcpOfRoleDTO{TargetRoleId("gripper")})),
            R"({"kind":"default_of_role","role":"gripper"})");
  EXPECT_EQ(canonical_json(EncodeTcpSelector(gripper_tcp())),
            R"({"entry":{"component_id":"cell.lite_gripper","frame_role_id":"tcp",)"
            R"("target_id":"gripper.main"},"kind":"entry"})");
  const auto entry = DecodeTcpEntry(EncodeTcpEntry(gripper_tcp()));
  ASSERT_TRUE(entry.ok());
  EXPECT_EQ(entry.value(), gripper_tcp());
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
}

}  // namespace
