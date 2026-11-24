#include <manyforge_core/testing/request_key_fixture.hpp>
#include "execution_host_commands.hpp"
#include "execution_host_posix.hpp"
#include "execution_host_responses.hpp"

#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <string>
#include <limits>
#include <stdexcept>
#include <manyforge_content_identity/content_identity.hpp>

namespace manyforge::roboplan_backend::host_detail {
namespace {
TEST(ComponentRecoveryCommandCodec, AcceptsExactFactsAndRejectsIncompleteConfirmation) {
  const auto preflight = DecodeCommand(
      R"({"api_version":"manyforge.execution_command.v4","command":"component_recovery_preflight","request":{"component_id":"arm"}})");
  EXPECT_EQ(preflight.kind, CommandKind::kComponentRecoveryPreflight);
  EXPECT_EQ(preflight.component_id, std::optional<std::string>("arm"));
  const auto confirmed = DecodeCommand(
      R"({"api_version":"manyforge.execution_command.v4","command":"component_action","request":{"action":"enable","component_id":"arm","recovery_preflight":{"confirmed":[{"cause_code":"fault_vendor","component_id":"arm","eligibility":"recover","stop_code":null,"stop_vendor_code":null,"vendor_code":"C31"}],"snapshot_id":"snapshot"}}})");
  ASSERT_TRUE(confirmed.recovery_preflight);
  // Served only on the component socket, never beside Stop.
  for (const auto kind : {preflight.kind, confirmed.kind, CommandKind::kReadComponentModel}) {
    EXPECT_TRUE(IsComponentLaneCommand(kind));
    EXPECT_FALSE(IsStopLaneRead(kind));
    EXPECT_FALSE(PreemptsPlanning(kind));
  }
  ASSERT_EQ(confirmed.recovery_preflight->confirmed.size(), 1U);
  EXPECT_EQ(confirmed.recovery_preflight->confirmed[0].vendor_code, std::optional<std::string>("C31"));
  EXPECT_THROW(DecodeCommand(
      R"({"api_version":"manyforge.execution_command.v4","command":"component_action","request":{"action":"enable","component_id":"arm","recovery_preflight":{"confirmed":[{"cause_code":null,"component_id":"arm","eligibility":"ready","vendor_code":null}],"snapshot_id":"snapshot"}}})"), std::exception);
}
using Value = content_identity::CanonicalValue;

TEST(ToolPayloadCommandCodec, RemovedBridgeCommandIsRejected) {
  const auto command = content_identity::canonical_json(Value(Value::Object{
      {"api_version", Value(std::string(kCommandApiVersion))},
      {"command", Value("apply_tool_payload")}}));
  EXPECT_THROW(DecodeCommand(command), std::exception);
}

// OD-341: a joint pose goal keeps its two-field shape; a linear one carries
// motion_type and the line settings, speed_mode optional.
TEST(MotionSpecCommandCodec, PoseGoalDecodesItsMotionType) {
  const Value goal(Value::Object{
      {"orientation", Value(Value::Object{{"w", Value(1.0)}, {"x", Value(0.0)},
                                          {"y", Value(0.0)}, {"z", Value(0.0)}})},
      {"position_m", Value(Value::Object{{"x_m", Value(0.3)}, {"y_m", Value(0.0)},
                                         {"z_m", Value(0.2)}})}});
  const auto joint = DecodeMotionSpec(Value(Value::Object{
      {"goal", goal}, {"kind", Value("pose_goal")}}));
  ASSERT_TRUE(std::holds_alternative<core::PoseGoalDTO>(joint));
  EXPECT_FALSE(std::get<core::PoseGoalDTO>(joint).linear.has_value());

  Value::Object line{{"goal", goal},
                     {"kind", Value("pose_goal")},
                     {"max_angular_acceleration_rad_s2", Value(1.0)},
                     {"max_angular_speed_rad_s", Value(0.4)},
                     {"max_linear_acceleration_mps2", Value(0.3)},
                     {"max_rotation_step_rad", Value(0.02)},
                     {"max_translation_step_m", Value(0.002)},
                     {"motion_type", Value("linear")}};
  const auto linear = DecodeMotionSpec(Value(line));
  const auto &decoded = std::get<core::PoseGoalDTO>(linear);
  ASSERT_TRUE(decoded.linear.has_value());
  EXPECT_DOUBLE_EQ(decoded.linear->max_translation_step_m, 0.002);
  EXPECT_DOUBLE_EQ(decoded.linear->max_rotation_step_rad, 0.02);
  EXPECT_DOUBLE_EQ(decoded.linear->max_angular_speed_rad_s, 0.4);
  EXPECT_DOUBLE_EQ(decoded.linear->max_linear_acceleration_mps2, 0.3);
  EXPECT_DOUBLE_EQ(decoded.linear->max_angular_acceleration_rad_s2, 1.0);
  EXPECT_FALSE(decoded.linear->speed_mode.has_value());

  auto timed = line;
  timed.emplace_back("speed_mode", Value("time_optimal"));
  EXPECT_EQ(std::get<core::PoseGoalDTO>(DecodeMotionSpec(Value(timed))).linear->speed_mode,
            core::CartesianSpeedModeDTO::kTimeOptimal);

  auto missing = line;
  missing.erase(std::remove_if(missing.begin(), missing.end(),
                               [](const auto &member) {
                                 return member.first == "max_rotation_step_rad";
                               }),
                missing.end());
  EXPECT_THROW(DecodeMotionSpec(Value(missing)), std::exception);
  auto joint_type = line;
  for (auto &member : joint_type)
    if (member.first == "motion_type") member.second = Value("joint");
  EXPECT_THROW(DecodeMotionSpec(Value(joint_type)), std::exception);
  auto bad_timing = line;
  bad_timing.emplace_back("speed_mode", Value("bounded"));
  EXPECT_THROW(DecodeMotionSpec(Value(bad_timing)), std::exception);
}

Command DecodeSetIoOutput(Value request) {
  return DecodeCommand(content_identity::canonical_json(Value(Value::Object{
      {"api_version", Value(std::string(kCommandApiVersion))},
      {"command", Value("set_io_output")},
      {"request", std::move(request)}})));
}

TEST(SetIoOutputCommandCodec, DecodesTheContractShape) {
  // As manyforge_headless_api/execution.py sends it.
  const auto command = DecodeSetIoOutput(Value(Value::Object{
      {"origin", Value("manual")}, {"signal_id", Value("lite6_robot_controller_io/CO1")}, {"value", Value(true)}}));
  EXPECT_EQ(command.kind, CommandKind::kSetIoOutput);
  EXPECT_TRUE(command.io_output_manual);
  ASSERT_TRUE(command.io_output_signal.has_value());
  EXPECT_EQ(command.io_output_signal->controllable_component_id.value(),
            "lite6_robot_controller_io");
  EXPECT_EQ(command.io_output_signal->name, "CO1");
  EXPECT_EQ(command.io_output_value, (std::variant<bool, double>(true)));
  const auto analog = DecodeSetIoOutput(Value(Value::Object{
      {"origin", Value("program")}, {"signal_id", Value("plc/AO0")}, {"value", Value(2.5)}}));
  EXPECT_EQ(analog.io_output_value, (std::variant<bool, double>(2.5)));
  EXPECT_FALSE(analog.io_output_manual);
}

TEST(SetIoOutputCommandCodec, RefusesMalformedRequests) {
  EXPECT_THROW(DecodeSetIoOutput(Value(Value::Object{
                   {"origin", Value("manual")}, {"signal_id", Value("no_slash")}, {"value", Value(true)}})),
               std::exception);
  EXPECT_THROW(DecodeSetIoOutput(Value(Value::Object{
                   {"origin", Value("manual")}, {"signal_id", Value("plc/")}, {"value", Value(true)}})),
               std::exception);
  EXPECT_THROW(DecodeSetIoOutput(Value(Value::Object{
                   {"origin", Value("manual")}, {"signal_id", Value("plc/DO0")}, {"value", Value("on")}})),
               std::exception);
  EXPECT_THROW(DecodeSetIoOutput(Value(Value::Object{{"signal_id", Value("plc/DO0")}})),
               std::exception);
  // `origin` is required and is `manual` or `program`.
  EXPECT_THROW(DecodeSetIoOutput(Value(Value::Object{
                   {"signal_id", Value("plc/DO0")}, {"value", Value(true)}})),
               std::exception);
  EXPECT_THROW(DecodeSetIoOutput(Value(Value::Object{
                   {"origin", Value("operator")}, {"signal_id", Value("plc/DO0")}, {"value", Value(true)}})),
               std::exception);
}

Value Row(std::string name, std::string type, Value value) {
  return Value(Value::Object{{"name", Value(std::move(name))},
                             {"value_type", Value(std::move(type))},
                             {"value", std::move(value)}});
}
Value Frame(Value::Array values = {}) {
  return Value(Value::Object{
      {"schema_version", Value("manyforge.program_bbkey_publication.v2")},
      {"program_binding_id", Value("actual-binding-required-by-Core")},
      {"active_publication_generation", Value(1.0)},
      {"cycle_generation", Value(1.0)},
      {"claim_generation", Value(nullptr)},
      {"program_run_id", Value(nullptr)},
      {"kind", Value("baseline")},
      {"message_sequence", Value("1")},
      {"previous_sequence", Value("0")},
      {"values", Value(std::move(values))}});
}
Value Replace(const Value &value, std::string key, Value replacement) {
  auto rows = value.as_object();
  for (auto &[name, member] : rows)
    if (name == key) {
      member = std::move(replacement);
      return Value(std::move(rows));
    }
  rows.emplace_back(std::move(key), std::move(replacement));
  return Value(std::move(rows));
}
Command DecodeFrame(Value frame) {
  return DecodeCommand(content_identity::canonical_json(Value(Value::Object{
      {"api_version", Value(std::string(kCommandApiVersion))},
      {"command", Value("publish_program_bbkeys")},
      {"attribution", Value(Value::Object{{"transport", Value("internal")},
                                          {"assurance", Value("unverified")},
                                          {"principal_id", Value(nullptr)}})},
      {"request", std::move(frame)}})));
}

// These tests qualify the untrusted wire boundary only. Decoding an identity
// does not install a source, grant its occurrence, or acknowledge a frame.
TEST(ProgramBlackboardCommandCodecTest,
     PreservesSignedExtremesAndCompleteString) {
  const std::string payload(8192U, 's');
  auto decoded =
      DecodeFrame(Frame({Row("max", "int64", Value("9223372036854775807")),
                         Row("min", "int64", Value("-9223372036854775808")),
                         Row("text", "string", Value(payload))}));
  ASSERT_EQ(decoded.kind, CommandKind::kPublishProgramBlackboard);
  ASSERT_TRUE(decoded.blackboard_publication);
  const auto &values = decoded.blackboard_publication->values;
  ASSERT_EQ(values.size(), 3U);
  EXPECT_EQ(std::get<std::int64_t>(*values[0].value),
            std::numeric_limits<std::int64_t>::max());
  EXPECT_EQ(std::get<std::int64_t>(*values[1].value),
            std::numeric_limits<std::int64_t>::min());
  EXPECT_EQ(std::get<std::string>(*values[2].value), payload);
  EXPECT_FALSE(decoded.blackboard_publication->scope.program_run_id);
  EXPECT_FALSE(decoded.blackboard_publication->scope.claim_generation);
  EXPECT_EQ(decoded.blackboard_publication->trusted_received_at_ns, 0);
}
TEST(ProgramBlackboardCommandCodecTest,
     UnsetRetainsDeclaredTypeWithoutDefault) {
  auto decoded = DecodeFrame(Frame({Row("boolean", "bool", Value(nullptr)),
                                    Row("integer", "int64", Value(nullptr)),
                                    Row("real", "float64", Value(nullptr)),
                                    Row("text", "string", Value(nullptr))}));
  ASSERT_TRUE(decoded.blackboard_publication);
  const auto &values = decoded.blackboard_publication->values;
  ASSERT_EQ(values.size(), 4U);
  EXPECT_EQ(values[0].value_kind, core::IoConditionValueKind::kBoolean);
  EXPECT_EQ(values[1].value_kind, core::IoConditionValueKind::kInt64);
  EXPECT_EQ(values[2].value_kind, core::IoConditionValueKind::kFloat64);
  EXPECT_EQ(values[3].value_kind, core::IoConditionValueKind::kString);
  for (const auto &value : values)
    EXPECT_FALSE(value.value);
}
TEST(ProgramBlackboardCommandCodecTest,
     RejectsIntegerCoercionOverflowAndMalleableDecimals) {
  for (const auto &bytes :
       {"+1", "01", "-0", "9223372036854775808", "-9223372036854775809"})
    EXPECT_THROW(DecodeFrame(Frame({Row("key", "int64", Value(bytes))})),
                 std::exception);
  EXPECT_THROW(DecodeFrame(Frame({Row("key", "int64", Value(1.0))})),
               std::exception);
  EXPECT_THROW(DecodeFrame(Frame({Row("key", "bool", Value(0.0))})),
               std::exception);
  EXPECT_THROW(DecodeFrame(Frame({Row("key", "float64", Value(true))})),
               std::exception);
}
TEST(ProgramBlackboardCommandCodecTest,
     RejectsDuplicateKeysUnknownFieldsRenewalAndPartialRun) {
  EXPECT_THROW(DecodeFrame(Frame({Row("a", "bool", Value(true)),
                                  Row("a", "bool", Value(false))})),
               std::exception);
  EXPECT_THROW(DecodeFrame(Frame({Row("z", "bool", Value(true)),
                                  Row("a", "bool", Value(false))})),
               std::exception);
  EXPECT_THROW(DecodeFrame(Replace(Frame(), "kind", Value("renewal"))),
               std::exception);
  EXPECT_THROW(DecodeFrame(Replace(Frame(), "source_epoch",
                                   Value("caller-grant-forbidden"))),
               std::exception);
  EXPECT_THROW(DecodeFrame(Replace(Frame(), "claim_generation", Value(1.0))),
               std::exception);
}
TEST(ProgramBlackboardCommandCodecTest,
     PreservesFullUnsignedWireSequenceWithoutDoubleCoercion) {
  auto frame = Replace(Frame(), "kind", Value("update"));
  frame = Replace(frame, "message_sequence", Value("18446744073709551615"));
  frame = Replace(frame, "previous_sequence", Value("18446744073709551614"));
  auto decoded = DecodeFrame(frame);
  ASSERT_TRUE(decoded.blackboard_publication);
  EXPECT_EQ(decoded.blackboard_publication->message_sequence,
            std::numeric_limits<std::uint64_t>::max());
  for (const auto &bytes : {"01", "+1", "18446744073709551616"})
    EXPECT_THROW(
        DecodeFrame(Replace(Frame(), "message_sequence", Value(bytes))),
        std::exception);
  EXPECT_THROW(DecodeFrame(Replace(Frame(), "message_sequence", Value(1.0))),
               std::exception);
}
Command DecodeCarriedObjectLost(Value level, Value attachment, Value object,
                                std::string transport = "internal",
                                Value loss_event_id = Value(nullptr),
                                Value set_key = Value(nullptr)) {
  return DecodeCommand(content_identity::canonical_json(Value(Value::Object{
      {"api_version", Value(std::string(kCommandApiVersion))},
      {"command", Value("set_carried_object_lost")},
      {"attribution", Value(Value::Object{{"transport", Value(transport)},
                                          {"assurance", Value("unverified")},
                                          {"principal_id", Value(nullptr)}})},
      {"request",
       Value(Value::Object{{"carried_object_attachment_id", std::move(attachment)},
                           {"command_target_id", Value("gripper")},
                           {"controllable_component_id", Value("robot.1")},
                           {"level", std::move(level)},
                           {"loss_event_id", std::move(loss_event_id)},
                           {"scene_resource_id", std::move(object)},
                           {"set_key", std::move(set_key)}})}})));
}

TEST(ProgramBlackboardCommandCodecTest,
     CarriedObjectLostLevelNamesItsAttachmentOnlyWhenTrue) {
  const auto lost = DecodeCarriedObjectLost(Value(true), Value("attachment.1"),
                                            Value("box"), "internal",
                                            Value("loss.7"), Value("part_lost"));
  ASSERT_EQ(lost.kind, CommandKind::kSetCarriedObjectLost);
  ASSERT_TRUE(lost.carried_object_lost_set.has_value());
  EXPECT_EQ(lost.carried_object_lost_set->controllable_component_id.value(),
            "robot.1");
  EXPECT_EQ(lost.carried_object_lost_set->command_target_id.value(), "gripper");
  EXPECT_TRUE(lost.carried_object_lost_set->level);
  EXPECT_EQ(lost.carried_object_lost_set->carried_object_attachment_id,
            std::optional<std::string>("attachment.1"));
  // The loss event id and the stored key travel with `true`.
  EXPECT_EQ(lost.carried_object_lost_set->loss_event_id,
            std::optional<std::string>("loss.7"));
  EXPECT_EQ(lost.carried_object_lost_set->set_key,
            std::optional<std::string>("part_lost"));
  // A key is optional with `true`; the loss event id is not.
  EXPECT_FALSE(DecodeCarriedObjectLost(Value(true), Value("attachment.1"),
                                       Value("box"), "internal", Value("loss.7"))
                   .carried_object_lost_set->set_key.has_value());
  EXPECT_THROW(DecodeCarriedObjectLost(Value(true), Value("attachment.1"),
                                       Value("box")),
               std::exception);
  // `false` names no loss event and no key.
  EXPECT_THROW(DecodeCarriedObjectLost(Value(false), Value(nullptr), Value(nullptr),
                                       "internal", Value("loss.7")),
               std::exception);
  EXPECT_THROW(DecodeCarriedObjectLost(Value(false), Value(nullptr), Value(nullptr),
                                       "internal", Value(nullptr), Value("part_lost")),
               std::exception);
  const auto cleared =
      DecodeCarriedObjectLost(Value(false), Value(nullptr), Value(nullptr));
  EXPECT_FALSE(cleared.carried_object_lost_set->level);
  EXPECT_FALSE(cleared.carried_object_lost_set->scene_resource_id.has_value());
  EXPECT_THROW(DecodeCarriedObjectLost(Value(true), Value(nullptr), Value("box"),
                                       "internal", Value("loss.7")),
               std::exception);
  EXPECT_THROW(
      DecodeCarriedObjectLost(Value(false), Value("attachment.1"), Value("box")),
      std::exception);
  EXPECT_THROW(DecodeCarriedObjectLost(Value(true), Value("attachment.1"),
                                       Value("box"), "public", Value("loss.7")),
               std::exception);
}

} // namespace
} // namespace manyforge::roboplan_backend::host_detail


namespace manyforge::roboplan_backend::host_detail {
namespace {
Value MockStartRow(std::string target, std::string resource,
                   Value change = Value(nullptr)) {
  return Value(Value::Object{
      {"signal", Value(Value::Object{{"component_id", Value(std::move(target))},
                                      {"signal_name", Value(std::move(resource))}})},
      {"start_value", Value(false)}, {"change", std::move(change)}});
}
Value MockStartRequest(Value::Array rows = {MockStartRow("arm", "ready")}) {
  return Value(Value::Object{
      {"schema_version", Value("manyforge.mock_io_node_start.v3")},
      {"program_binding_id", Value("binding.1")},
      {"claim_generation", Value(1.0)}, {"program_run_id", Value(1.0)},
      {"node_instance_id", Value("wait")}, {"node_attempt_id", Value(1.0)},
      {"node_start_index", Value("1")}, {"signals", Value(std::move(rows))}});
}
Value MockStartEnvelope(Value request) {
  return Value(Value::Object{
      {"api_version", Value(std::string(kCommandApiVersion))},
      {"command", Value("start_mock_io_node_simulation")},
      {"attribution", Value(Value::Object{{"transport", Value("internal")},
                                          {"assurance", Value("unverified")},
                                          {"principal_id", Value(nullptr)}})},
      {"request", std::move(request)}});
}
Command DecodeMockStart(Value request) {
  return DecodeCommand(content_identity::canonical_json(MockStartEnvelope(std::move(request))));
}
TEST(ProgramMockNodeStartCodecTest, PreservesFullWidthIndexAndLiteralRowsWithoutNewCaps) {
  const auto change = Value(Value::Object{{"after_ms", Value(0.0)}, {"value", Value(true)}});
  const auto decoded = DecodeMockStart(Replace(
      MockStartRequest({MockStartRow("arm", "ready", change)}), "node_start_index",
      Value("18446744073709551615")));
  ASSERT_EQ(decoded.kind, CommandKind::kStartMockIoNodeSimulation);
  ASSERT_TRUE(decoded.mock_node_simulation);
  const auto& request = *decoded.mock_node_simulation;
  EXPECT_EQ(request.scope.node_start_index, std::numeric_limits<std::uint64_t>::max());
  EXPECT_EQ(request.scope.program_run_id.value(), 1U);
  EXPECT_EQ(request.scope.node.node_attempt_id, core::NodeAttemptNumber(1U));
  ASSERT_EQ(request.signals.size(), 1U);
  EXPECT_FALSE(request.signals.front().start_value);
  ASSERT_TRUE(request.signals.front().change);
  EXPECT_EQ(request.signals.front().change->after_ms, 0U);
  EXPECT_TRUE(request.signals.front().change->value);
  EXPECT_FALSE(DecodeMockStart(MockStartRequest()).mock_node_simulation->signals.front().change);
  // Wire representability is distinct from the runtime's checked deadline.
  // No previously proposed 16-selector / one-hour policy cap is inferred.
  Value::Array rows;
  for (unsigned i = 0; i < 17U; ++i)
    rows.push_back(MockStartRow("arm", "input." + std::to_string(100U + i)));
  EXPECT_EQ(DecodeMockStart(MockStartRequest(std::move(rows)))
                .mock_node_simulation->signals.size(), 17U);
  const auto long_offset = Value(Value::Object{{"after_ms", Value(3'600'001.0)},
                                               {"value", Value(false)}});
  EXPECT_EQ(DecodeMockStart(MockStartRequest({MockStartRow("arm", "ready", long_offset)}))
                .mock_node_simulation->signals.front().change->after_ms, 3'600'001U);
}
TEST(ProgramMockNodeStartCodecTest, RejectsCoercionNoncanonicalIndexAndUntrustedAttribution) {
  for (const auto& value : {Value("0"), Value("01"), Value("+1"), Value("-1"),
                             Value("1.0"), Value("1e0"), Value(""),
                             Value("18446744073709551616"), Value(1.0),
                             Value(true), Value(nullptr)}) {
    SCOPED_TRACE(content_identity::canonical_json(value));
    EXPECT_THROW(DecodeMockStart(Replace(MockStartRequest(), "node_start_index", value)),
                 std::exception);
  }
  for (const auto& value : {Value(-1.0), Value(0.5), Value("0"), Value(true)}) {
    const auto change = Value(Value::Object{{"after_ms", value}, {"value", Value(true)}});
    EXPECT_THROW(DecodeMockStart(MockStartRequest({MockStartRow("arm", "ready", change)})),
                 std::exception);
  }
  for (const auto& value : {Value(0.0), Value("false"), Value(nullptr)})
    EXPECT_THROW(DecodeMockStart(MockStartRequest({Replace(MockStartRow("arm", "ready"),
                                                           "start_value", value)})), std::exception);
  for (const auto& attribution : {
           Value(Value::Object{{"transport", Value("socket")}, {"assurance", Value("unverified")},
                               {"principal_id", Value(nullptr)}}),
           Value(Value::Object{{"transport", Value("internal")}, {"assurance", Value("unverified")},
                               {"principal_id", Value("caller")}})})
    EXPECT_THROW(DecodeCommand(content_identity::canonical_json(Replace(
                     MockStartEnvelope(MockStartRequest()), "attribution", attribution))), std::exception);
}
TEST(ProgramMockNodeStartCodecTest, RejectsOpenShapesPhysicalAuthorityAndSelectorConflicts) {
  for (const auto& request : {
           Replace(MockStartRequest(), "schema_version", Value("manyforge.mock_io_node_start.v2")),
           Replace(MockStartRequest(), "previous_node_attempt_id", Value(2.0)),
           Replace(MockStartRequest(), "source_incarnation", Value("caller-authority")),
           Replace(MockStartRequest(), "node_attempt_id", Value(0.0)),
           Replace(MockStartRequest(), "node_attempt_id", Value("1")),
           MockStartRequest({}),
           MockStartRequest({MockStartRow("arm", "ready"), MockStartRow("arm", "ready")}),
           MockStartRequest({MockStartRow("z", "ready"), MockStartRow("a", "ready")}),
           MockStartRequest({Replace(MockStartRow("arm", "ready"), "catalog_generation", Value(1.0))}),
           MockStartRequest({Replace(MockStartRow("arm", "ready"), "signal",
               Value(Value::Object{{"component_id", Value("arm")}, {"signal_name", Value("ready")},
                                   {"controllable_component_id", Value("caller")}}))}),
           MockStartRequest({MockStartRow("arm", "ready", Value(Value::Object{
               {"after_ms", Value(0.0)}, {"value", Value(true)}, {"repeat", Value(true)}}))})}) {
    EXPECT_THROW(DecodeMockStart(request), std::exception);
  }
  auto missing_change = MockStartRow("arm", "ready").as_object();
  std::erase_if(missing_change, [](const auto& row) { return row.first == "change"; });
  EXPECT_THROW(DecodeMockStart(MockStartRequest({Value(std::move(missing_change))})), std::exception);
  EXPECT_THROW(DecodeCommand(content_identity::canonical_json(Replace(
                   MockStartEnvelope(MockStartRequest()), "source_owner", Value("caller")))), std::exception);
}
}  // namespace
}  // namespace manyforge::roboplan_backend::host_detail

namespace manyforge::roboplan_backend::host_detail {
namespace {
Value MockEndRequest() {
  auto rows = MockStartRequest().as_object();
  std::erase_if(rows, [](const auto& row) { return row.first == "signals"; });
  return Replace(Value(std::move(rows)), "schema_version", Value("manyforge.mock_io_node_end.v2"));
}
Command DecodeMockEnd(Value request) {
  return DecodeCommand(content_identity::canonical_json(Replace(
      MockStartEnvelope(std::move(request)), "command", Value("end_mock_io_node_simulation"))));
}
TEST(ProgramMockNodeEndCodecTest, PreservesExactAttemptAndFullWidthIndexWithoutStartPayload) {
  const auto decoded = DecodeMockEnd(Replace(MockEndRequest(), "node_start_index",
                                              Value("18446744073709551615")));
  ASSERT_EQ(decoded.kind, CommandKind::kEndMockIoNodeSimulation);
  ASSERT_TRUE(decoded.mock_node_simulation_end);
  EXPECT_FALSE(decoded.mock_node_simulation);
  const auto& scope = *decoded.mock_node_simulation_end;
  EXPECT_EQ(scope.node_start_index, std::numeric_limits<std::uint64_t>::max());
  EXPECT_EQ(scope.node.node_instance_id.value(), "wait");
  EXPECT_EQ(scope.node.node_attempt_id, core::NodeAttemptNumber(1U));
  EXPECT_EQ(scope.program_run_id.value(), 1U);
  EXPECT_EQ(scope.claim_generation.value(), 1U);
}
TEST(ProgramMockNodeEndCodecTest, RejectsRowsAuthorityCoercionAndOpenOrIncompleteScope) {
  for (const auto& value : {Value("0"), Value("01"), Value("+1"), Value("-1"),
                            Value("18446744073709551616"), Value(1.0), Value(true)})
    EXPECT_THROW(DecodeMockEnd(Replace(MockEndRequest(), "node_start_index", value)), std::exception);
  for (const auto& request : {
      Replace(MockEndRequest(), "signals", Value(Value::Array{MockStartRow("arm", "ready")})),
      Replace(MockEndRequest(), "started_at_ns", Value("123")),
      Replace(MockEndRequest(), "source_incarnation", Value("caller")),
      Replace(MockEndRequest(), "schema_version", Value("manyforge.mock_io_node_start.v3")),
      Replace(MockEndRequest(), "node_attempt_id", Value(0.0)),
      Replace(MockEndRequest(), "node_attempt_id", Value("1"))})
    EXPECT_THROW(DecodeMockEnd(request), std::exception);
  auto missing = MockEndRequest().as_object();
  std::erase_if(missing, [](const auto& row) { return row.first == "program_run_id"; });
  EXPECT_THROW(DecodeMockEnd(Value(std::move(missing))), std::exception);
  const auto foreign_attribution = Value(Value::Object{
      {"transport", Value("internal")}, {"assurance", Value("unverified")},
      {"principal_id", Value("caller")}});
  const auto envelope = Replace(Replace(MockStartEnvelope(MockEndRequest()), "command",
      Value("end_mock_io_node_simulation")), "attribution", foreign_attribution);
  EXPECT_THROW(DecodeCommand(content_identity::canonical_json(envelope)), std::exception);
}
}  // namespace
}  // namespace manyforge::roboplan_backend::host_detail

namespace manyforge::roboplan_backend::host_detail {
namespace {
Value MockRunPauseRequest() {
  return Value(Value::Object{
      {"schema_version", Value("manyforge.mock_io_run_pause.v2")},
      {"program_binding_id", Value("binding.1")},
      {"claim_generation", Value(1.0)}, {"program_run_id", Value(1.0)},
      {"run_revision", Value("1")}, {"paused", Value(false)},
      {"held_node_instance_ids", Value(Value::Array{})}});
}
Command DecodeMockRunPause(Value request) {
  return DecodeCommand(content_identity::canonical_json(Replace(
      MockStartEnvelope(std::move(request)), "command", Value("set_mock_io_run_paused"))));
}
TEST(ProgramMockRunPauseCodecTest, PreservesExplicitFalseTrueAndFullWidthRunnerRevision) {
  const auto running = DecodeMockRunPause(Replace(MockRunPauseRequest(), "run_revision",
                                                  Value("18446744073709551615")));
  ASSERT_EQ(running.kind, CommandKind::kSetMockIoRunPaused);
  ASSERT_TRUE(running.mock_run_simulation_pause);
  EXPECT_EQ(running.mock_run_simulation_pause->run_revision, std::numeric_limits<std::uint64_t>::max());
  EXPECT_FALSE(running.mock_run_simulation_pause->paused);
  EXPECT_EQ(running.mock_run_simulation_pause->scope.program_run_id.value(), 1U);
  EXPECT_FALSE(running.mock_node_simulation);
  EXPECT_FALSE(running.mock_node_simulation_end);
  const auto paused = DecodeMockRunPause(Replace(MockRunPauseRequest(), "paused", Value(true)));
  ASSERT_TRUE(paused.mock_run_simulation_pause);
  EXPECT_TRUE(paused.mock_run_simulation_pause->paused);
}
TEST(ProgramMockRunPauseCodecTest, RejectsInferredRunningCoercionNodePayloadAndUntrustedAttribution) {
  for (const auto& value : {Value("0"), Value("01"), Value("+1"),
                            Value("18446744073709551616"), Value(1.0), Value(true)})
    EXPECT_THROW(DecodeMockRunPause(Replace(MockRunPauseRequest(), "run_revision", value)), std::exception);
  for (const auto& value : {Value("false"), Value(0.0), Value(nullptr)})
    EXPECT_THROW(DecodeMockRunPause(Replace(MockRunPauseRequest(), "paused", value)), std::exception);
  for (const auto& request : {
      Replace(MockRunPauseRequest(), "node_instance_id", Value("wait")),
      Replace(MockRunPauseRequest(), "node_start_index", Value("1")),
      Replace(MockRunPauseRequest(), "signals", Value(Value::Array{MockStartRow("arm", "ready")})),
      Replace(MockRunPauseRequest(), "source_incarnation", Value("caller")),
      Replace(MockRunPauseRequest(), "schema_version", Value("manyforge.mock_io_run_pause.v1"))})
    EXPECT_THROW(DecodeMockRunPause(request), std::exception);
  auto missing = MockRunPauseRequest().as_object();
  std::erase_if(missing, [](const auto& row) { return row.first == "paused"; });
  EXPECT_THROW(DecodeMockRunPause(Value(std::move(missing))), std::exception);
  const auto foreign_attribution = Value(Value::Object{
      {"transport", Value("internal")}, {"assurance", Value("unverified")},
      {"principal_id", Value("caller")}});
  const auto envelope = Replace(Replace(MockStartEnvelope(MockRunPauseRequest()), "command",
      Value("set_mock_io_run_paused")), "attribution", foreign_attribution);
  EXPECT_THROW(DecodeCommand(content_identity::canonical_json(envelope)), std::exception);
}
}  // namespace
}  // namespace manyforge::roboplan_backend::host_detail

namespace manyforge::roboplan_backend::host_detail {
namespace {
TEST(ProgramMockRunPauseCodecTest, RequiresExplicitCanonicalHeldNodeMaskInExistingRunSnapshot) {
  const Value mask(Value::Array{Value("move"), Value("wait")});
  const auto decoded = DecodeMockRunPause(Replace(MockRunPauseRequest(), "held_node_instance_ids", mask));
  ASSERT_TRUE(decoded.mock_run_simulation_pause);
  EXPECT_EQ(decoded.mock_run_simulation_pause->held_node_instance_ids,
      (std::vector<core::ProgramNodeInstanceId>{core::ProgramNodeInstanceId("move"),
                                                core::ProgramNodeInstanceId("wait")}));
  const auto empty = DecodeMockRunPause(MockRunPauseRequest());
  ASSERT_TRUE(empty.mock_run_simulation_pause);
  EXPECT_TRUE(empty.mock_run_simulation_pause->held_node_instance_ids.empty());
  for (const auto& invalid : {
      Value(nullptr), Value("wait"), Value(true),
      Value(Value::Array{Value(1.0)}), Value(Value::Array{Value("")}),
      Value(Value::Array{Value(std::string(129U, 'n'))}),
      Value(Value::Array{Value(std::string("wait\0late", 9U))}),
      Value(Value::Array{Value("wait"), Value("move")}),
      Value(Value::Array{Value("wait"), Value("wait")})}) {
    SCOPED_TRACE(content_identity::canonical_json(invalid));
    EXPECT_THROW(DecodeMockRunPause(Replace(MockRunPauseRequest(), "held_node_instance_ids", invalid)),
                 std::exception);
  }
  auto missing = MockRunPauseRequest().as_object();
  std::erase_if(missing, [](const auto& row) { return row.first == "held_node_instance_ids"; });
  EXPECT_THROW(DecodeMockRunPause(Value(std::move(missing))), std::exception);
  // This is one ordered Run snapshot, never a separate caller-selected clock.
  EXPECT_THROW(DecodeMockRunPause(Replace(MockRunPauseRequest(), "selection_revision", Value("1"))),
               std::exception);
}
TEST(VolatileMotionTraceCommandTest, OrdinaryReadHasBoundedAllowanceAndNoControlLaneEffects) {
  const auto request = [](double allowance) {
    return content_identity::canonical_json(Value(Value::Object{
        {"api_version", Value("manyforge.execution_command.v4")},
        {"command", Value("read_volatile_motion_trace")},
        {"allowance_bytes", Value(allowance)},
        {"attribution", Value(Value::Object{
            {"transport", Value("internal")}, {"assurance", Value("unverified")},
            {"principal_id", Value(nullptr)}})}}));
  };
  const auto command = DecodeCommand(request(5242880.0));
  EXPECT_EQ(command.kind, CommandKind::kReadVolatileMotionTrace);
  EXPECT_EQ(command.trace_allowance_bytes, 5242880U);
  EXPECT_FALSE(IsSubmissionOrdered(command.kind));
  EXPECT_FALSE(ReadsManagerScene(command.kind));
  EXPECT_FALSE(IsStopLaneRead(command.kind));
  EXPECT_FALSE(IsComponentLaneCommand(command.kind));
  EXPECT_FALSE(PreemptsPlanning(command.kind));
  for (const double invalid : {0.0, -1.0, 0.5, 5242881.0})
    EXPECT_THROW((void)DecodeCommand(request(invalid)), std::exception);
}

// The trace export broke once a trace outgrew the default unix send buffer
// (212992 bytes): the listener's SO_SNDBUF never reaches an accepted socket,
// so the host's send failed and the client saw an empty response. The
// capacity is configured and measured on the accepted client itself, and a
// response of exactly that size crosses the real socket in one packet.
TEST(VolatileMotionTraceCommandTest, AcceptedClientCarriesItsAtomicResponseCapacity) {
  const auto root = std::filesystem::temp_directory_path() /
                    ("mf-response-capacity-" + std::to_string(::getpid()));
  std::filesystem::create_directories(root);
  {
    const CommandSocket listener(root, "capacity.sock");
    FileDescriptor client(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0));
    ASSERT_TRUE(client);
    const int receive_bytes = static_cast<int>(kMaximumResponseBytes);
    ASSERT_EQ(::setsockopt(client.get(), SOL_SOCKET, SO_RCVBUF, &receive_bytes,
                           sizeof(receive_bytes)), 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const auto path = (root / "capacity.sock").string();
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1U);
    ASSERT_EQ(::connect(client.get(), reinterpret_cast<sockaddr *>(&address),
                        sizeof(address)), 0);
    FileDescriptor accepted(::accept4(listener.get(), nullptr, nullptr, SOCK_CLOEXEC));
    ASSERT_TRUE(accepted);
    int initial = 0;
    socklen_t initial_size = sizeof(initial);
    ASSERT_EQ(::getsockopt(accepted.get(), SOL_SOCKET, SO_SNDBUF, &initial,
                           &initial_size), 0);

    const auto capacity = AtomicResponseCapacity(accepted.get());
    EXPECT_GT(capacity, static_cast<std::size_t>(initial) - 32U);
    EXPECT_LE(capacity, kMaximumResponseBytes);
    const std::string payload(capacity, 'x');
    ASSERT_EQ(::send(accepted.get(), payload.data(), payload.size(), MSG_NOSIGNAL),
              static_cast<ssize_t>(payload.size()));
    std::string received(kMaximumResponseBytes, '\0');
    EXPECT_EQ(::recv(client.get(), received.data(), received.size(), 0),
              static_cast<ssize_t>(payload.size()));
    if (capacity < kMaximumResponseBytes) {
      const std::string oversized(capacity + 1U, 'x');
      EXPECT_EQ(::send(accepted.get(), oversized.data(), oversized.size(), MSG_NOSIGNAL), -1);
      EXPECT_EQ(errno, EMSGSIZE);
    }
  }
  std::filesystem::remove_all(root);
}
}  // namespace

// Active TCP: the motion's TCP is the three-form selector; the
// older `{target_role_id, frame_role_id}` endpoint is refused.
TEST(MotionEndpointCommandCodecTest,
     DecodesOnlyTheRequiredTcpSelector) {
  const auto payload = Value(Value::Object{
      {"kind", Value("motion")},
      {"move_id", Value("move.endpoint")},
      {"base_frame", Value(Value::Object{{"component_id", Value("robot")}, {"frame_id", Value("base")}})},
      {"tcp_endpoint", Value(Value::Object{{"kind", Value("current")}})},
      {"tolerances",
       Value(Value::Object{{"joint_position_tol_rad", Value(0.01)},
                           {"translation_tol_m", Value(0.001)},
                           {"rotation_tol_rad", Value(0.01)}})},
      {"speed_profile",
       Value(Value::Object{{"velocity_scale", Value(1.0)},
                           {"acceleration_scale", Value(1.0)},
                           {"max_tcp_linear_speed_mps", Value(nullptr)}})},
      {"timeouts", Value(Value::Object{{"planning_timeout_s", Value(2.0)},
                                       {"execution_timeout_s", Value(5.0)}})},
      {"cache_mode", Value("default")},
      {"target_mode", Value("absolute")},
      {"target_reference", Value(nullptr)},
      {"offset_reference", Value(nullptr)},
      {"collision_rules", Value(Value::Array{})},
      {"per_joint_dynamics_limits", Value(Value::Object{})},
      {"motion_interrupt_scopes", Value(Value::Array{})},
      {"motion", Value(Value::Object{
                     {"kind", Value("joint_goal")},
                     {"joint_names", Value(Value::Array{Value("joint")})},
                     {"positions_rad", Value(Value::Array{Value(0.2)})}})}});
  const auto decode = [&](Value motion) {
    return DecodeOperation(
        Value(Value::Object{
            {"request_context",
             Value(Value::Object{{"request_key", Value(core::RequestKeyText(
                                                     core::TestRequestKey("request.endpoint")))},
                                 {"source_label", Value("test")}})},
            {"program_revision", Value("revision")},
            {"program_binding_id", Value("binding")},
            {"program_run_id", Value(107.0)},
            {"expected_graph_revision", Value("graph")},
            {"expected_retained_bundle_id", Value("bundle")},
            {"expected_scene_revision", Value(nullptr)},
            {"coordination_policy", Value("independent")},
            {"participants", Value(Value::Array{Value(Value::Object{
                                 {"target", Value(Value::Object{{"component_id", Value("robot")}, {"command_target_id", Value("arm")}})},
                                 {"required_capability_id",
                                  Value("planning.motion.joint_goal")},
                                 {"action_span", Value(nullptr)},
                                 {"payload", std::move(motion)}})})}}),
        core::RequestAttributionDTO{});
  };
  const auto own = decode(payload);
  EXPECT_TRUE(std::holds_alternative<core::CurrentTcpDTO>(
      std::get<core::MotionParticipantIntentDTO>(own.participants.front().payload)
          .tcp_endpoint));
  auto fields = payload.as_object();
  const Value target(Value::Object{{"component_id", Value("gripper")}, {"command_target_id", Value("tool")}});
  const Value frame(Value::Object{{"component_id", Value("gripper")}, {"frame_id", Value("tip")}});
  const Value endpoint(Value::Object{{"kind", Value("entry")}, {"target", target}, {"frame", frame}});
  const auto qualified =
      decode(Replace(Value(fields), "tcp_endpoint", endpoint));
  const auto &motion = std::get<core::MotionParticipantIntentDTO>(
      qualified.participants.front().payload);
  EXPECT_EQ(motion.tcp_endpoint,
            core::TcpSelectorDTO(core::TcpEntryReferenceDTO{{core::ControllableComponentId("gripper"),
                                                   core::CommandTargetId("tool")},
                                                  {core::ControllableComponentId("gripper"), core::FrameRoleId("tip")}}));
  const auto form2 = decode(Replace(
      Value(fields), "tcp_endpoint",
      Value(Value::Object{{"kind", Value("default_of_target")}, {"target", target}})));
  EXPECT_EQ(std::get<core::MotionParticipantIntentDTO>(form2.participants.front().payload)
                .tcp_endpoint,
            core::TcpSelectorDTO(core::DefaultTcpOfTargetDTO{{core::ControllableComponentId("gripper"), core::CommandTargetId("tool")}}));
  // The older endpoint shape is refused.
  EXPECT_THROW(decode(Replace(Value(fields), "tcp_endpoint",
                              Value(Value::Object{{"target_role_id", Value("arm")},
                                                  {"frame_role_id", Value("tcp")}}))),
               std::exception);
  EXPECT_TRUE(std::get<core::MotionParticipantIntentDTO>(own.participants.front().payload)
                  .collision_rules.empty());
  // Collision rules travel with the motion, a joint destination included.
  // Resolving their selections remains Core-owned.
  const Value tcp_bodies(Value::Object{{"kind", Value("active_tcp_contact_bodies")}});
  const Value probe_block(Value::Object{{"kind", Value("scene_object")},
                                        {"scene_resource_id", Value("probe_block")}});
  const Value release(Value::Object{
      {"release_evidence_mode", Value("signed_penetration")},
      {"enforce_max_start_penetration", Value(true)},
      {"max_start_penetration_m", Value(0.002)},
      {"release_tolerance_m", Value(1e-5)},
      {"min_release_progress_m", Value(0.0)},
      {"require_separation_by_end", Value(true)},
      {"require_strictly_decreasing_penetration", Value(true)},
      {"allow_recontact", Value(false)}});
  const Value release_rule(Value::Object{
      {"rule_id", Value("withdrawal")},
      {"a", Value(Value::Array{tcp_bodies})},
      {"b", Value(Value::Array{probe_block})},
      {"release", release}});
  const Value contact_rule(Value::Object{
      {"rule_id", Value("gripper_on_parts")},
      {"a", Value(Value::Array{
                Value(Value::Object{{"kind", Value("collision_body")},
                                    {"component_id", Value("gripper")},
                                    {"collision_body_id", Value("gripper_collision")}}),
                Value(Value::Object{{"kind", Value("link")},
                                    {"component_id", Value("robot")},
                                    {"link", Value("link6")}}),
                Value(Value::Object{{"kind", Value("component")},
                                    {"component_id", Value("gripper")}})})},
      {"b", Value(Value::Array{
                Value(Value::Object{{"kind", Value("attached_object")},
                                    {"component_id", Value("gripper")}}),
                Value(Value::Object{{"kind", Value("scene_objects")}})})},
      {"contact", Value(Value::Object{{"max_penetration_m", Value(0.002)},
                                      {"end_from_fraction", Value(0.8)}})}});
  const Value everything_else(Value::Object{{"rule_id", Value("everything_else")},
                                            {"clearance_m", Value(0.002)}});
  const auto withdrawal = Replace(
      payload, "collision_rules",
      Value(Value::Array{everything_else, release_rule, contact_rule}));
  const auto decoded_withdrawal = decode(withdrawal);
  const auto &ruled = std::get<core::MotionParticipantIntentDTO>(
      decoded_withdrawal.participants.front().payload);
  ASSERT_EQ(ruled.collision_rules.size(), 3U);
  EXPECT_EQ(ruled.collision_rules[0],
            (core::CollisionRuleDTO{"everything_else", {}, {},
                                    core::CollisionClearanceRule{0.002}}));
  core::CollisionReleaseRule expected_release;
  expected_release.max_start_penetration_m = 0.002;
  EXPECT_EQ(ruled.collision_rules[1],
            (core::CollisionRuleDTO{
                "withdrawal",
                {core::CollisionRuleActiveTcpContactBodiesSelection{}},
                {core::CollisionRuleSceneObjectSelection{core::SceneResourceId("probe_block")}},
                expected_release}));
  EXPECT_EQ(ruled.collision_rules[2],
            (core::CollisionRuleDTO{
                "gripper_on_parts",
                {core::CollisionRuleCollisionBodySelection{
                     {core::ControllableComponentId("gripper"),
                      core::CollisionBodyRoleId("gripper_collision")}},
                 core::CollisionRuleLinkSelection{core::ControllableComponentId("robot"),
                                                  "link6"},
                 core::CollisionRuleComponentSelection{
                     core::ControllableComponentId("gripper")}},
                {core::CollisionRuleAttachedObjectSelection{
                     core::ControllableComponentId("gripper")},
                 core::CollisionRuleSceneObjectsSelection{}},
                core::CollisionContactRule{0.002, 0.8, std::nullopt}}));
  // A contact value may leave out either member: no depth check, the default
  // end window.
  const auto defaulted = decode(Replace(
      payload, "collision_rules",
      Value(Value::Array{Replace(contact_rule, "contact", Value(Value::Object{}))})));
  EXPECT_EQ(std::get<core::CollisionContactRule>(
                std::get<core::MotionParticipantIntentDTO>(
                    defaulted.participants.front().payload)
                    .collision_rules.front()
                    .value),
            core::CollisionContactRule{});
  const auto refused_rule = [&](const char *name, Value rule) {
    SCOPED_TRACE(name);
    EXPECT_THROW(decode(Replace(payload, "collision_rules",
                                Value(Value::Array{std::move(rule)}))),
                 std::exception);
  };
  // One value per row; `a` and `b` together; closed rows and selections.
  refused_rule("two values", Replace(everything_else, "contact", Value(Value::Object{})));
  refused_rule("no value", Value(Value::Object{{"rule_id", Value("no_value")}}));
  refused_rule("a without b", Value(Value::Object{{"rule_id", Value("only_a")},
                                   {"a", Value(Value::Array{tcp_bodies})},
                                   {"clearance_m", Value(0.002)}}));
  refused_rule("unknown row member", Replace(release_rule, "leg", Value("retreat")));
  refused_rule("unknown selection member", Replace(release_rule, "a",
                       Value(Value::Array{Replace(tcp_bodies, "extra", Value(true))})));
  refused_rule("empty side", Replace(release_rule, "a", Value(Value::Array{})));
  refused_rule("scene object by blackboard key", Replace(release_rule, "b",
                       Value(Value::Array{Value(Value::Object{
                           {"kind", Value("scene_object")},
                           {"blackboard_key", Value("trial.probe_object")}})})));
  refused_rule("model group on a move", Replace(release_rule, "b",
                       Value(Value::Array{Value(Value::Object{
                           {"kind", Value("model_group")},
                           {"component_id", Value("robot")},
                           {"group", Value("arm")}})})));
  refused_rule("unknown release evidence mode", Replace(release_rule, "release",
                       Replace(release, "release_evidence_mode", Value("force"))));
  refused_rule("unknown contact member", Replace(contact_rule, "contact",
                       Value(Value::Object{{"terminal_window_start_fraction", Value(0.8)}})));
  // Values outside their range and repeated ids are refused by the shared
  // shape check.
  refused_rule("clearance out of range", Replace(everything_else, "clearance_m", Value(0.06)));
  refused_rule("contact end fraction out of range", Replace(contact_rule, "contact",
                       Value(Value::Object{{"end_from_fraction", Value(1.5)}})));
  EXPECT_THROW(decode(Replace(payload, "collision_rules",
                              Value(Value::Array{release_rule, release_rule}))),
               std::exception);
  // The removed fields are unknown fields.
  EXPECT_THROW(decode(Replace(payload, "contact_policy_id", Value(nullptr))),
               std::exception);
  EXPECT_THROW(decode(Replace(payload, "clearance_m", Value(0.002))),
               std::exception);
  // Runtime-object bindings serve target references; a joint destination
  // has none, and spatial references stay invalid for it.
  const Value runtime_object(Value::Object{
      {"runtime_object_id", Value("selected_object")},
      {"scene_resource_id", Value("probe_block")}});
  EXPECT_THROW(decode(Replace(withdrawal, "runtime_object_bindings",
                              Value(Value::Array{runtime_object}))),
               std::exception);
  const Value reference(Value::Object{{"type", Value("cell_frame")}});
  for (const auto &mode : {"absolute", "relative"}) {
    for (const auto &field : {"target_reference", "offset_reference"}) {
      EXPECT_THROW(decode(Replace(Replace(withdrawal, "target_mode", Value(mode)),
                                  field, reference)),
                   std::exception);
    }
  }
  EXPECT_THROW(
      decode(Replace(Value(fields), "tcp_frame_role_id", Value("tcp"))),
      std::exception);
  EXPECT_THROW(decode(Replace(Value(fields), "tcp_endpoint",
                              Replace(endpoint, "extra", Value(true)))),
               std::exception);
  EXPECT_THROW(decode(Replace(Value(fields), "tcp_endpoint",
                              Replace(endpoint, "frame",
                                      Replace(frame, "frame_id", Value(""))))),
               std::exception);
  // The planner block: absent or null keeps the Deployment's; set members
  // override; unknown members and out-of-range values are refused.
  EXPECT_EQ(std::get<core::MotionParticipantIntentDTO>(own.participants.front().payload)
                .planner,
            core::PlannerOverrideDTO{});
  EXPECT_EQ(std::get<core::MotionParticipantIntentDTO>(
                decode(Replace(payload, "planner", Value(nullptr))).participants.front().payload)
                .planner,
            core::PlannerOverrideDTO{});
  const Value planner(Value::Object{{"search", Value("rrt")},
                                    {"optimize", Value("best_of_n")},
                                    {"n", Value(5.0)},
                                    {"max_nodes", Value(nullptr)}});
  const auto planned = decode(Replace(payload, "planner", planner));
  EXPECT_EQ(std::get<core::MotionParticipantIntentDTO>(planned.participants.front().payload)
                .planner,
            (core::PlannerOverrideDTO{core::PlannerSearchDTO::kRrt,
                                      core::PlannerOptimizeDTO::kBestOfN, 5U,
                                      std::nullopt, std::nullopt}));
  EXPECT_THROW(decode(Replace(payload, "planner", Replace(planner, "extra", Value(true)))),
               std::exception);
  EXPECT_THROW(decode(Replace(payload, "planner", Replace(planner, "n", Value(1.0)))),
               std::exception);
  EXPECT_THROW(decode(Replace(payload, "planner",
                              Replace(planner, "optimize", Value("balanced")))),
               std::exception);
  std::erase_if(fields,
                [](const auto &row) { return row.first == "tcp_endpoint"; });
  EXPECT_THROW(decode(Value(fields)), std::exception);
}

// Active TCP: the Set TCP payload carries a selector only.
TEST(MotionEndpointCommandCodecTest, DecodesTheSetTcpPayload) {
  const auto decode = [](Value payload) {
    return DecodeOperation(
        Value(Value::Object{
            {"request_context",
             Value(Value::Object{{"request_key", Value(core::RequestKeyText(
                                                     core::TestRequestKey("request.set_tcp")))},
                                 {"source_label", Value("test")}})},
            {"program_revision", Value("revision")},
            {"program_binding_id", Value("binding")},
            {"program_run_id", Value(107.0)},
            {"expected_graph_revision", Value("graph")},
            {"expected_retained_bundle_id", Value("bundle")},
            {"expected_scene_revision", Value(nullptr)},
            {"coordination_policy", Value("independent")},
            {"participants", Value(Value::Array{Value(Value::Object{
                                 {"target", Value(Value::Object{{"component_id", Value("robot")}, {"command_target_id", Value("arm")}})},
                                 {"required_capability_id",
                                  Value("execution.trajectory.execute")},
                                 {"action_span", Value(nullptr)},
                                 {"payload", std::move(payload)}})})}}),
        core::RequestAttributionDTO{});
  };
  const Value target(Value::Object{{"component_id", Value("gripper")}, {"command_target_id", Value("tool")}});
  const Value frame(Value::Object{{"component_id", Value("gripper")}, {"frame_id", Value("tip")}});
  const auto decoded = decode(Value(Value::Object{
      {"kind", Value("set_tcp")},
      {"tcp", Value(Value::Object{{"kind", Value("entry")}, {"target", target}, {"frame", frame}})}}));
  const auto &set = std::get<core::SetTcpParticipantIntentDTO>(
      decoded.participants.front().payload);
  EXPECT_TRUE(std::holds_alternative<core::TcpEntryReferenceDTO>(set.tcp));
  EXPECT_THROW(decode(Value(Value::Object{{"kind", Value("set_tcp")}})), std::exception);
  EXPECT_THROW(decode(Value(Value::Object{
                   {"kind", Value("set_tcp")},
                   {"tcp", Value(Value::Object{{"kind", Value("current")}})},
                   {"extra", Value(true)}})),
               std::exception);
}

TEST(MockIoInputCommandCodecTest,
     MalformedFinalBooleanPreservesOwningCleanupAndValidationOrder) {
  const Value request(Value::Object{
      {"schema_version", Value("manyforge.mock_io_input_request.v3")},
      {"program_binding_id", Value("binding.io-wire")},
      {"signal", Value(Value::Object{{"component_id", Value("io_component")},
                                    {"signal_name", Value("ready_input")}})},
      {"expected_source_epoch", Value("installation.io-wire")},
      {"value", Value("false")}});
  const auto decode = [](Value input) {
    return DecodeCommand(content_identity::canonical_json(Value(Value::Object{
        {"api_version", Value(std::string(kCommandApiVersion))},
        {"command", Value("set_mock_io_input")},
        {"attribution", Value(Value::Object{{"transport", Value("internal")},
                                            {"assurance", Value("unverified")},
                                            {"principal_id", Value(nullptr)}})},
        {"request", std::move(input)}})));
  };
  // This exact malformed last field previously double-destroyed the nested
  // query's heap-backed owner during exception cleanup on the qualified GCC.
  try {
    static_cast<void>(decode(request));
    FAIL() << "a string Boolean must be refused";
  } catch (const std::runtime_error &error) {
    EXPECT_STREQ(error.what(),
                 "Execution host: command field value must be boolean");
  }
  for (const bool value : {false, true}) {
    const auto decoded = decode(Replace(request, "value", Value(value)));
    EXPECT_EQ(decoded.kind, CommandKind::kSetMockIoInput);
    ASSERT_TRUE(decoded.mock_io_input_set);
    const auto &input = *decoded.mock_io_input_set;
    EXPECT_EQ(input.query.program_binding_id,
              core::ProgramBindingId("binding.io-wire"));
    ASSERT_TRUE(input.query.signal);
    EXPECT_EQ(input.query.signal->component_id, core::ControllableComponentId("io_component"));
    EXPECT_EQ(input.query.signal->signal_name, "ready_input");
    EXPECT_FALSE(input.query.frame);
    EXPECT_FALSE(input.query.relative_to_frame);
    EXPECT_FALSE(input.query.frame_transform);
    EXPECT_EQ(input.expected_source_epoch,
              core::IoSourceIncarnation("installation.io-wire"));
    EXPECT_EQ(input.value, value);
  }
  // The last string field still takes precedence over the malformed Boolean.
  try {
    static_cast<void>(
        decode(Replace(request, "expected_source_epoch", Value(""))));
    FAIL() << "an empty source epoch must be refused";
  } catch (const std::runtime_error &error) {
    EXPECT_STREQ(error.what(),
                 "Execution host: command field expected_source_epoch "
                 "must be one bounded string");
  }
}

TEST(OperationRecoveryCommandCodecTest,
     ExactFencesAndOptionalFreshAttemptStayClosed) {
  const Value request(Value::Object{
      {"request_key", Value(core::RequestKeyText(core::TestRequestKey("recovery.codec.request")))},
      {"action", Value("reset_cycle")},
      {"principal_id", Value("operator.codec")},
      {"expected_assessment_revision", Value(7.0)},
      {"expected_control_command_epoch", Value(3.0)},
      {"expected_execution_id", Value(42.0)},
      {"reason", Value("codec validation only")}});
  const auto decode = [](Value selected) {
    return DecodeCommand(content_identity::canonical_json(Value(
        Value::Object{{"api_version", Value(std::string(kCommandApiVersion))},
                      {"command", Value("apply_operation_recovery")},
                      {"operation_id", Value(1001.0)},
                      {"expected_operation_revision", Value(5.0)},
                      {"request", std::move(selected)}})));
  };
  const auto omitted = decode(request);
  ASSERT_TRUE(omitted.operation_recovery);
  EXPECT_EQ(omitted.kind, CommandKind::kApplyOperationRecovery);
  ASSERT_TRUE(omitted.operation_id);
  EXPECT_EQ(*omitted.operation_id, core::OperationNumber(1001U));
  ASSERT_TRUE(omitted.expected_operation_revision);
  EXPECT_EQ(*omitted.expected_operation_revision, core::OperationRevision(5U));
  const auto &captured = *omitted.operation_recovery;
  EXPECT_EQ(captured.action, core::RecoveryAction::kResetCycle);
  EXPECT_EQ(captured.expected_assessment_revision, 7U);
  EXPECT_EQ(captured.expected_control_command_epoch, 3U);
  EXPECT_EQ(captured.expected_execution_id, 42U);
  EXPECT_FALSE(captured.fresh_motion_attempt);
  for (const bool fresh : {false, true}) {
    const auto decoded =
        decode(Replace(request, "fresh_motion_attempt", Value(fresh)));
    ASSERT_TRUE(decoded.operation_recovery);
    EXPECT_EQ(decoded.operation_recovery->fresh_motion_attempt, fresh);
    EXPECT_EQ(decoded.operation_recovery->expected_execution_id,
              captured.expected_execution_id);
  }
  for (const auto &malformed :
       {Value(nullptr), Value(0.0), Value(1.0), Value("false"),
        Value(Value::Array{}), Value(Value::Object{})})
    EXPECT_THROW(decode(Replace(request, "fresh_motion_attempt", malformed)),
                 std::exception);
  EXPECT_THROW(decode(Replace(request, "unknown_field", Value(false))),
               std::exception);
  // The removed Stop-certificate fences are unknown members now.
  EXPECT_THROW(decode(Replace(request, "expected_stop_certificate_id",
                              Value("certificate.codec"))),
               std::exception);
  EXPECT_THROW(
      decode(Replace(request, "action", Value("report_final_motion_failure"))),
      std::exception);
}

TEST(OperationRecoveryCommandCodecTest,
     AssessmentAndRejectedActionKeepTypedWireEvidence) {
  core::KernelRecoveryAssessmentDTO assessment;
  assessment.assessment_revision = 7U;
  assessment.control_command_epoch = 3U;
  assessment.execution_id = 42U;
  assessment.execution_state = core::ExecutionState::kCompleted;
  assessment.reason_code = "codec_validation_only";
  const auto encoded = RecoveryAssessmentValue(assessment);
  ASSERT_NE(encoded.find_object_member("schema_version"), nullptr);
  EXPECT_EQ(*encoded.find_object_member("schema_version"),
            Value("manyforge.kernel_recovery_assessment.v2"));
  // The Stop facts of the new state model replace the certificate members.
  EXPECT_EQ(*encoded.find_object_member("stop_event_id"), Value(0.0));
  EXPECT_EQ(*encoded.find_object_member("stop_phase"), Value(""));
  EXPECT_EQ(*encoded.find_object_member("containing_stage_id"), Value(""));
  for (const auto *removed :
       {"stop_certificate_id", "stop_certificate_revision", "stop_owner_generation"})
    EXPECT_EQ(encoded.find_object_member(removed), nullptr) << removed;
  const auto *eligibility = encoded.find_object_member("retry_eligibility");
  ASSERT_NE(eligibility, nullptr);
  EXPECT_EQ(*eligibility,
            Value(Value::Object{{"cause", Value("not_eligible")},
                                {"effect", Value("effect_uncertain")},
                                {"eligible", Value(false)},
                                {"reason", Value("not_retryable_state")}}));
  const core::KernelRecoveryActionResultDTO rejected{
      core::TestRequestKey("recovery.codec.request"), core::RecoveryAction::kResetCycle, false,
      assessment,
      core::ErrorDTO{"recovery_rejected",
                     "reset belongs to Program control",
                     {{"reason", "action_not_allowed"}}}};
  const auto result = RecoveryActionResultValue(rejected);
  ASSERT_TRUE(result.is_object());
  EXPECT_EQ(result.as_object().size(), 5U);
  EXPECT_EQ(*result.find_object_member("accepted"), Value(false));
  EXPECT_EQ(*result.find_object_member("action"), Value("reset_cycle"));
  EXPECT_EQ(*result.find_object_member("resulting_assessment"), encoded);
  const auto *error = result.find_object_member("error");
  ASSERT_NE(error, nullptr);
  EXPECT_EQ(*error->find_object_member("code"), Value("recovery_rejected"));
  const auto *details = error->find_object_member("details");
  ASSERT_NE(details, nullptr);
  EXPECT_EQ(*details->find_object_member("reason"),
            Value("action_not_allowed"));
}

TEST(ActiveTcpCommandCodecTest, OrderedReadRoutesOnlyExplicitTrueToSubmissionLane) {
  const auto decode = [](Value::Object extra) {
    Value::Object root{{"api_version", Value(std::string(kCommandApiVersion))},
                       {"command", Value("read_active_tcp")}};
    root.insert(root.end(), extra.begin(), extra.end());
    return DecodeCommand(content_identity::canonical_json(Value(std::move(root))));
  };
  const auto ordinary = decode({});
  EXPECT_EQ(ordinary.kind, CommandKind::kReadActiveTcp);
  EXPECT_FALSE(IsSubmissionOrdered(ordinary.kind));
  const auto explicitly_false = decode({{"ordered", Value(false)}});
  EXPECT_EQ(explicitly_false.kind, CommandKind::kReadActiveTcp);
  EXPECT_FALSE(IsSubmissionOrdered(explicitly_false.kind));
  const auto ordered = decode({{"ordered", Value(true)}});
  EXPECT_EQ(ordered.kind, CommandKind::kReadActiveTcpOrdered);
  EXPECT_TRUE(IsSubmissionOrdered(ordered.kind));
  EXPECT_THROW((void)decode({{"ordered", Value("true")}}), std::exception);
  EXPECT_THROW((void)decode({{"ordered", Value(true)}, {"extra", Value(true)}}), std::exception);
}

TEST(RuntimeScaleCommandRoutingTest, SpeedScalingTakesNoSubmissionOrdering) {
  // It runs on its own lane, never behind planning or Submit.
  EXPECT_FALSE(IsSubmissionOrdered(CommandKind::kSetRuntimeScaleControl));
  EXPECT_FALSE(PreemptsPlanning(CommandKind::kSetRuntimeScaleControl));
  EXPECT_TRUE(IsSubmissionOrdered(CommandKind::kSubmit));
}


Command DecodeStopCommand(const std::string_view name, Value attribution, Value request) {
  return DecodeCommand(content_identity::canonical_json(Value(Value::Object{
      {"api_version", Value(std::string(kCommandApiVersion))},
      {"attribution", std::move(attribution)},
      {"command", Value(std::string(name))},
      {"request", std::move(request)}})));
}

Value OperatorAttribution() {
  return Value(Value::Object{{"assurance", Value("externally_authenticated")},
                             {"principal_id", Value("operator.1")},
                             {"transport", Value("http")}});
}

Value SelectionRequest() {
  return Value(Value::Object{
      {"group_ids", Value(Value::Array{Value("all")})},
      {"program_run_id", Value(1.0)},
      {"request_context",
       Value(Value::Object{
           {"request_key", Value(core::RequestKeyText(core::TestRequestKey("stop.group")))},
           {"source_label", Value("headless")}})},
      {"schema_version", Value("manyforge.program_execution_selection.v3")}});
}

// K-10: the selection Stop takes any caller and keeps its principal for the
// Stop record. Stop by run stays internal only (contract 1.8, "Runner → host
// (internal only)"); Run and Resume stay the Program owner's.
TEST(ProgramStopCommandCodec, SelectionStopKeepsAnOperatorPrincipalAndRunStopStaysInternal) {
  const auto selection =
      DecodeStopCommand("stop_program_selection", OperatorAttribution(), SelectionRequest());
  EXPECT_EQ(selection.kind, CommandKind::kStopProgramSelection);
  ASSERT_TRUE(selection.program_selection.has_value());
  EXPECT_EQ(selection.program_selection->attribution.transport, core::RequestTransportDTO::kHttp);
  EXPECT_EQ(selection.program_selection->attribution.principal_id,
            std::optional<std::string>("operator.1"));

  EXPECT_THROW((void)DecodeStopCommand(
                   "stop_program_run", OperatorAttribution(),
                   Value(Value::Object{
                       {"program_run_id", Value(1.0)},
                       {"request_context",
                        Value(Value::Object{
                            {"request_key",
                             Value(core::RequestKeyText(core::TestRequestKey("stop.run")))},
                            {"source_label", Value("headless")}})}})),
               std::exception);

  for (const auto* command : {"start_program_selection", "release_program_selection"}) {
    SCOPED_TRACE(command);
    EXPECT_THROW(DecodeCommand(content_identity::canonical_json(Value(Value::Object{
                     {"api_version", Value(std::string(kCommandApiVersion))},
                     {"attribution", OperatorAttribution()},
                     {"command", Value(command)},
                     {"expected_hold_revision", Value(1.0)},
                     {"request", SelectionRequest()}}))),
                 std::exception);
  }
}

namespace {
// Stop state model (2026-11-13): the host codec of the Stop forms, their
// replies and the stream/control facts that replace the run fence.

Value InternalAttribution() {
  return Value(Value::Object{{"assurance", Value("unverified")},
                             {"principal_id", Value(nullptr)},
                             {"transport", Value("internal")}});
}

Value StopRequestContext(const std::string_view key) {
  return Value(Value::Object{
      {"request_key", Value(core::RequestKeyText(core::TestRequestKey(std::string(key))))},
      {"source_label", Value("headless")}});
}

Command DecodeRoot(Value::Object root) {
  root.emplace_back("api_version", Value(std::string(kCommandApiVersion)));
  return DecodeCommand(content_identity::canonical_json(Value(std::move(root))));
}

Value ProgramNodeStopRequest() {
  auto request = SelectionRequest().as_object();
  request.emplace_back("stopped_by_node_instance_id", Value("ask"));
  return Value(std::move(request));
}

// OD-342: the Program owner's own group Stop names the requesting node; a
// client cannot, and Run/Resume never carry it.
TEST(ProgramStopCommandCodec, AProgramNodeStopNamesItsNodeFromTheProgramOwnerOnly) {
  const auto decoded =
      DecodeStopCommand("stop_program_selection", InternalAttribution(), ProgramNodeStopRequest());
  ASSERT_TRUE(decoded.program_selection.has_value());
  EXPECT_EQ(decoded.program_selection->stopped_by_node_instance_id,
            std::optional<core::ProgramNodeInstanceId>(core::ProgramNodeInstanceId("ask")));
  EXPECT_FALSE(
      DecodeStopCommand("stop_program_selection", InternalAttribution(), SelectionRequest())
          .program_selection->stopped_by_node_instance_id.has_value());
  EXPECT_THROW((void)DecodeStopCommand("stop_program_selection", OperatorAttribution(),
                                       ProgramNodeStopRequest()),
               std::exception);
  EXPECT_THROW((void)DecodeRoot({{"attribution", InternalAttribution()},
                                 {"command", Value("release_program_selection")},
                                 {"expected_hold_revision", Value(1.0)},
                                 {"request", ProgramNodeStopRequest()}}),
               std::exception);
}

std::vector<std::string> MemberNames(const Value &value) {
  std::vector<std::string> names;
  for (const auto &[name, member] : value.as_object()) names.push_back(name);
  std::sort(names.begin(), names.end());
  return names;
}

std::vector<std::string> Sorted(std::vector<std::string> names) {
  std::sort(names.begin(), names.end());
  return names;
}

TEST(StopCommandCodec, CellWideFormsDecodeOneRequestContextBody) {
  for (const auto &[name, kind] :
       {std::pair{"stop_all", CommandKind::kStopAll}, std::pair{"halt", CommandKind::kHalt},
        std::pair{"cell_stop", CommandKind::kCellStop}}) {
    SCOPED_TRACE(name);
    const auto body = Value(Value::Object{{"request_context", StopRequestContext(name)}});
    const auto decoded = DecodeStopCommand(name, InternalAttribution(), body);
    EXPECT_EQ(decoded.kind, kind);
    EXPECT_TRUE(PreemptsPlanning(decoded.kind));
    ASSERT_TRUE(decoded.cell_wide_stop.has_value());
    EXPECT_EQ(decoded.cell_wide_stop->request_context.request_key,
              core::TestRequestKey(name));
    EXPECT_EQ(decoded.cell_wide_stop->request_context.source_label, "headless");
    EXPECT_EQ(decoded.cell_wide_stop->attribution, core::RequestAttributionDTO{});
    EXPECT_FALSE(decoded.stop.has_value());
    EXPECT_FALSE(decoded.program_run_end.has_value());
    // The body names no target and no fence.
    EXPECT_THROW((void)DecodeStopCommand(
                     name, InternalAttribution(),
                     Replace(body, "expected_stop_generation", Value(0.0))),
                 std::exception);
    EXPECT_THROW((void)DecodeStopCommand(name, InternalAttribution(),
                                         Replace(body, "program_run_id", Value(1.0))),
                 std::exception);
    EXPECT_THROW((void)DecodeStopCommand(name, InternalAttribution(), Value(Value::Object{})),
                 std::exception);
  }
  // Stop all and Halt take any caller and keep its principal.
  for (const auto *name : {"stop_all", "halt"}) {
    SCOPED_TRACE(name);
    const auto decoded = DecodeStopCommand(
        name, OperatorAttribution(),
        Value(Value::Object{{"request_context", StopRequestContext(name)}}));
    ASSERT_TRUE(decoded.cell_wide_stop.has_value());
    EXPECT_EQ(decoded.cell_wide_stop->attribution.principal_id,
              std::optional<std::string>("operator.1"));
  }
}

TEST(StopCommandCodec, CellStopIsTheRunnersInternalPrimitiveOnly) {
  const auto body =
      Value(Value::Object{{"request_context", StopRequestContext("cell.stop")}});
  EXPECT_THROW((void)DecodeStopCommand("cell_stop", OperatorAttribution(), body),
               std::exception);
  // Internal transport with a principal.
  EXPECT_THROW((void)DecodeStopCommand(
                   "cell_stop",
                   Value(Value::Object{{"assurance", Value("unverified")},
                                       {"principal_id", Value("runner.1")},
                                       {"transport", Value("internal")}}),
                   body),
               std::exception);
  // Not internal, even unverified and without a principal.
  for (const auto *transport : {"http", "ros"}) {
    SCOPED_TRACE(transport);
    EXPECT_THROW((void)DecodeStopCommand(
                     "cell_stop",
                     Value(Value::Object{{"assurance", Value("unverified")},
                                         {"principal_id", Value(nullptr)},
                                         {"transport", Value(transport)}}),
                     body),
                 std::exception);
  }
  // Internal but externally authenticated.
  EXPECT_THROW((void)DecodeStopCommand(
                   "cell_stop",
                   Value(Value::Object{{"assurance", Value("externally_authenticated")},
                                       {"principal_id", Value(nullptr)},
                                       {"transport", Value("internal")}}),
                   body),
               std::exception);
}

TEST(StopCommandCodec, ProgramRunStopBodyIsExactlyContextAndRun) {
  const auto body = Value(Value::Object{{"program_run_id", Value(9.0)},
                                        {"request_context", StopRequestContext("run.end")}});
  const auto decoded = DecodeStopCommand("stop_program_run", InternalAttribution(), body);
  EXPECT_EQ(decoded.kind, CommandKind::kStopProgramRun);
  EXPECT_TRUE(PreemptsPlanning(decoded.kind));
  ASSERT_TRUE(decoded.program_run_end.has_value());
  EXPECT_EQ(decoded.program_run_end->program_run_id, core::ProgramRunNumber(9U));
  EXPECT_EQ(decoded.program_run_end->request_context.request_key,
            core::TestRequestKey("run.end"));
  EXPECT_EQ(decoded.program_run_end->attribution, core::RequestAttributionDTO{});
  for (const auto &[member, extra] :
       {std::pair{"reason", Value("operator")},
        std::pair{"group_ids", Value(Value::Array{Value("all")})},
        std::pair{"expected_stop_generation", Value(0.0)}}) {
    SCOPED_TRACE(member);
    EXPECT_THROW((void)DecodeStopCommand("stop_program_run", InternalAttribution(),
                                         Replace(body, member, extra)),
                 std::exception);
  }
  EXPECT_THROW((void)DecodeStopCommand(
                   "stop_program_run", InternalAttribution(),
                   Value(Value::Object{{"request_context", StopRequestContext("run.end")}})),
               std::exception);
  EXPECT_THROW((void)DecodeStopCommand("stop_program_run", InternalAttribution(),
                                       Replace(body, "program_run_id", Value(0.0))),
               std::exception);
}

TEST(StopCommandCodec, OperationTargetHasTwoMembers) {
  const auto target = Value(Value::Object{{"expected_operation_revision", Value(2.0)},
                                          {"operation_id", Value(5.0)}});
  const auto body = [](Value row) {
    return Value(Value::Object{{"operations", Value(Value::Array{std::move(row)})},
                               {"request_context", StopRequestContext("stop.operation")}});
  };
  const auto decoded = DecodeStopCommand("stop", OperatorAttribution(), body(target));
  EXPECT_EQ(decoded.kind, CommandKind::kStop);
  ASSERT_TRUE(decoded.stop.has_value());
  const auto *operations =
      std::get_if<std::vector<core::StopOperationTargetDTO>>(&decoded.stop->selector);
  ASSERT_NE(operations, nullptr);
  EXPECT_EQ(*operations, (std::vector<core::StopOperationTargetDTO>{
                             {core::OperationNumber(5U), core::OperationRevision(2U)}}));
  EXPECT_FALSE(decoded.stop->reason.has_value());
  EXPECT_EQ(decoded.stop->attribution.principal_id, std::optional<std::string>("operator.1"));
  // The former Stop generation (or any third member) is refused.
  EXPECT_THROW((void)DecodeStopCommand("stop", OperatorAttribution(),
                                       body(Replace(target, "expected_stop_generation",
                                                    Value(0.0)))),
               std::exception);
  const auto with_reason = DecodeStopCommand(
      "stop", OperatorAttribution(), Replace(body(target), "reason", Value("jam")));
  ASSERT_TRUE(with_reason.stop.has_value());
  EXPECT_EQ(with_reason.stop->reason, std::optional<std::string>("jam"));
}

TEST(StopCommandCodec, ReleaseHaltNamesRunAndHoldRevision) {
  const Value::Object root{{"attribution", InternalAttribution()},
                           {"command", Value("release_halt")},
                           {"expected_hold_revision", Value(4.0)},
                           {"program_run_id", Value(3.0)}};
  const auto decoded = DecodeRoot(root);
  EXPECT_EQ(decoded.kind, CommandKind::kReleaseHalt);
  EXPECT_EQ(decoded.program_run_id, std::optional(core::ProgramRunNumber(3U)));
  EXPECT_EQ(decoded.expected_hold_revision, 4U);
  auto with_zero = root;
  with_zero[2].second = Value(0.0);
  EXPECT_EQ(DecodeRoot(with_zero).expected_hold_revision, 0U);
  auto operator_root = root;
  operator_root[0].second = OperatorAttribution();
  EXPECT_THROW((void)DecodeRoot(operator_root), std::exception);
  auto missing = root;
  missing.erase(missing.begin() + 2);
  EXPECT_THROW((void)DecodeRoot(missing), std::exception);
  auto extra = root;
  extra.emplace_back("expected_stop_generation", Value(0.0));
  EXPECT_THROW((void)DecodeRoot(extra), std::exception);
}

TEST(StopCommandCodec, ReadStopNamesOneEventId) {
  const Value::Object root{{"attribution", InternalAttribution()},
                           {"command", Value("read_stop")},
                           {"event_id", Value(1000.0)}};
  const auto decoded = DecodeRoot(root);
  EXPECT_EQ(decoded.kind, CommandKind::kReadStop);
  EXPECT_EQ(decoded.stop_event_id, std::optional(core::StopEventId(1000U)));
  EXPECT_TRUE(IsStopLaneRead(decoded.kind));
  EXPECT_FALSE(IsComponentLaneCommand(decoded.kind));
  EXPECT_FALSE(PreemptsPlanning(decoded.kind));
  auto zero = root;
  zero[2].second = Value(0.0);
  EXPECT_THROW((void)DecodeRoot(zero), std::exception);
  auto renamed = root;
  renamed[2].first = "stop_id";
  EXPECT_THROW((void)DecodeRoot(renamed), std::exception);
  auto operator_root = root;
  operator_root[0].second = OperatorAttribution();
  EXPECT_THROW((void)DecodeRoot(operator_root), std::exception);
}

TEST(StopCommandCodec, RecognizedStopKnowsThreeRoutes) {
  const auto root = [](const std::string_view route) {
    return Value::Object{
        {"attribution", OperatorAttribution()},
        {"command", Value("recognized_stop")},
        {"reason", Value("recognized_stop_payload_invalid")},
        {"recognized_route", Value(std::string(route))},
        {"request_key", Value(core::RequestKeyText(core::TestRequestKey("malformed")))},
        {"source_label", Value(nullptr)}};
  };
  for (const auto *route :
       {"/api/execution/stops", "/api/execution/stop-all", "/api/execution/halt"}) {
    SCOPED_TRACE(route);
    const auto decoded = DecodeRoot(root(route));
    EXPECT_EQ(decoded.kind, CommandKind::kRecognizedStop);
    EXPECT_TRUE(PreemptsPlanning(decoded.kind));
    ASSERT_TRUE(decoded.recognized_stop.has_value());
    EXPECT_EQ(decoded.recognized_stop->recognized_route, route);
    EXPECT_EQ(decoded.recognized_stop->request_key, core::TestRequestKey("malformed"));
    EXPECT_EQ(decoded.recognized_stop->reason, "recognized_stop_payload_invalid");
    EXPECT_FALSE(decoded.recognized_stop->source_label.has_value());
  }
  {
    // The Stop of another application start carries the caller's label.
    auto labelled = root("/api/execution/stops");
    for (auto& [name, value] : labelled) {
      if (name == "reason") value = Value("recognized_stop_start_not_current");
      if (name == "source_label") value = Value("operator_ui");
    }
    const auto decoded = DecodeRoot(labelled);
    ASSERT_TRUE(decoded.recognized_stop.has_value());
    EXPECT_EQ(decoded.recognized_stop->reason, "recognized_stop_start_not_current");
    EXPECT_EQ(decoded.recognized_stop->source_label, std::optional<std::string>("operator_ui"));
  }
  {
    // The old command name is gone (no alias), and the label member is required.
    auto renamed = root("/api/execution/stops");
    for (auto& [name, value] : renamed)
      if (name == "command") value = Value("malformed_stop");
    EXPECT_THROW((void)DecodeRoot(renamed), std::exception);
    auto without_label = root("/api/execution/stops");
    std::erase_if(without_label, [](const auto& member) { return member.first == "source_label"; });
    EXPECT_THROW((void)DecodeRoot(without_label), std::exception);
  }
  for (const auto *route : {"/api/execution/full-stop", "/api/execution/unknown"}) {
    SCOPED_TRACE(route);
    EXPECT_THROW((void)DecodeRoot(root(route)), std::exception);
  }
}

TEST(StopCommandCodec, RemovedStopCommandsAreUnknown) {
  EXPECT_THROW((void)DecodeStopCommand(
                   "full_stop", InternalAttribution(),
                   Value(Value::Object{{"request_context", StopRequestContext("full")}})),
               std::exception);
  EXPECT_THROW((void)DecodeRoot(Value::Object{{"attribution", InternalAttribution()},
                                              {"command", Value("release_program_run_stop_fence")},
                                              {"expected_stop_generation", Value(1.0)},
                                              {"program_run_id", Value(1.0)}}),
               std::exception);
  EXPECT_THROW((void)DecodeRoot(Value::Object{{"attribution", InternalAttribution()},
                                              {"command", Value("release_program_run_stop_fence")},
                                              {"program_run_id", Value(1.0)}}),
               std::exception);
}

TEST(StopCommandCodec, SelectionStartAndReleaseNameTheHoldRevisionAtTheRoot) {
  for (const auto &[name, kind] :
       {std::pair{"start_program_selection", CommandKind::kStartProgramSelection},
        std::pair{"release_program_selection", CommandKind::kReleaseProgramSelection}}) {
    SCOPED_TRACE(name);
    const auto decoded = DecodeRoot(Value::Object{{"attribution", InternalAttribution()},
                                                  {"command", Value(name)},
                                                  {"expected_hold_revision", Value(6.0)},
                                                  {"request", SelectionRequest()}});
    EXPECT_EQ(decoded.kind, kind);
    EXPECT_EQ(decoded.expected_hold_revision, 6U);
    ASSERT_TRUE(decoded.program_selection.has_value());
    EXPECT_EQ(decoded.program_selection->program_run_id, core::ProgramRunNumber(1U));
    EXPECT_THROW((void)DecodeRoot(Value::Object{{"attribution", InternalAttribution()},
                                                {"command", Value(name)},
                                                {"expected_selection_revision", Value(6.0)},
                                                {"request", SelectionRequest()}}),
                 std::exception);
  }
}

core::StopAcceptedDTO AcceptedFixture() {
  core::StopAcceptedDTO accepted;
  accepted.request_key = core::TestRequestKey("stop.accepted");
  accepted.replay_status = core::RequestReplayStatusDTO::kAcceptedNew;
  accepted.event_id = core::StopEventId(12U);
  accepted.requested = core::StopKind::kOperation;
  accepted.applied = core::StopKind::kStopAll;
  accepted.widen_reason = core::StopWidenReason::kUnknownTarget;
  accepted.stale_diagnostics = {core::StopStaleDiagnosticDTO{
      core::OperationNumber(5U), core::OperationRevision(2U), core::OperationRevision(3U)}};
  accepted.stale_total = 4U;
  accepted.covered = {core::StopCoveredParticipantDTO{core::OperationNumber(6U), 1U, true},
                      core::StopCoveredParticipantDTO{core::OperationNumber(7U), 0U, false}};
  accepted.hold_revision = 8U;
  accepted.accepted_at_ns = 1234;
  return accepted;
}

TEST(StopResponseCodec, StopAcceptedHasTheExactMemberSet) {
  auto accepted = AcceptedFixture();
  auto value = StopAcceptedValue(accepted);
  EXPECT_EQ(MemberNames(value),
            Sorted({"accepted_at_ns", "applied", "covered", "event_id", "hold_revision",
                    "program_run_id", "replay_status", "request_key", "requested",
                    "stale_diagnostics", "stale_total", "widen_reason"}));
  EXPECT_EQ(value.find_object_member("accepted_at_ns")->as_number(), 1234.0);
  EXPECT_EQ(value.find_object_member("applied")->as_string(), "stop_all");
  EXPECT_EQ(value.find_object_member("requested")->as_string(), "operation");
  EXPECT_EQ(value.find_object_member("widen_reason")->as_string(), "unknown_target");
  EXPECT_EQ(value.find_object_member("event_id")->as_number(), 12.0);
  EXPECT_EQ(value.find_object_member("hold_revision")->as_number(), 8.0);
  EXPECT_TRUE(value.find_object_member("program_run_id")->is_null());
  EXPECT_EQ(value.find_object_member("replay_status")->as_string(),
            core::to_string(core::RequestReplayStatusDTO::kAcceptedNew));
  EXPECT_EQ(value.find_object_member("request_key")->as_string(),
            core::RequestKeyText(core::TestRequestKey("stop.accepted")));
  EXPECT_EQ(value.find_object_member("stale_total")->as_number(), 4.0);
  const auto &covered = value.find_object_member("covered")->as_array();
  ASSERT_EQ(covered.size(), 2U);
  EXPECT_EQ(MemberNames(covered[0]),
            Sorted({"operation_id", "participant_ordinal", "started"}));
  EXPECT_EQ(covered[0].find_object_member("operation_id")->as_number(), 6.0);
  EXPECT_EQ(covered[0].find_object_member("participant_ordinal")->as_number(), 1.0);
  EXPECT_TRUE(covered[0].find_object_member("started")->as_bool());
  EXPECT_FALSE(covered[1].find_object_member("started")->as_bool());
  const auto &stale = value.find_object_member("stale_diagnostics")->as_array();
  ASSERT_EQ(stale.size(), 1U);
  EXPECT_EQ(MemberNames(stale[0]), Sorted({"current_operation_revision",
                                           "expected_operation_revision", "operation_id"}));
  EXPECT_EQ(stale[0].find_object_member("expected_operation_revision")->as_number(), 2.0);
  EXPECT_EQ(stale[0].find_object_member("current_operation_revision")->as_number(), 3.0);
  accepted.program_run_id = core::ProgramRunNumber(9U);
  value = StopAcceptedValue(accepted);
  EXPECT_EQ(value.find_object_member("program_run_id")->as_number(), 9.0);
}

TEST(StopResponseCodec, SelectionStopCarriesScopeAndStopAccepted) {
  core::ProgramExecutionSelectionAcceptedDTO selection;
  selection.selected_node_indices = {1U, 3U};
  selection.widened_node_indices = {1U, 2U, 3U};
  selection.stop = AcceptedFixture();
  auto value = ProgramSelectionAcceptedValue(selection, {"left", "right"});
  EXPECT_EQ(MemberNames(value),
            Sorted({"schema_version", "state", "group_ids", "selected_node_indices",
                    "widened_node_indices", "stop"}));
  EXPECT_EQ(value.find_object_member("schema_version")->as_string(),
            "manyforge.program_execution_selection.v3");
  EXPECT_TRUE(value.find_object_member("state")->is_null());
  EXPECT_EQ(value.find_object_member("group_ids")->as_array().size(), 2U);
  EXPECT_EQ(value.find_object_member("widened_node_indices")->as_array().size(), 3U);
  EXPECT_EQ(*value.find_object_member("stop"), StopAcceptedValue(selection.stop));
  selection.state = core::ProgramExecutionScopeStateDTO{
      core::CellAdmissionClaimGeneration(1U), core::ProgramRunNumber(9U), 8U, {}, {}};
  value = ProgramSelectionAcceptedValue(selection, {"left"});
  EXPECT_EQ(*value.find_object_member("state"), ProgramExecutionScopeValue(*selection.state));
}

TEST(StopResponseCodec, StopEventStatusPendingAndComplete) {
  core::StopEventStatusDTO status;
  status.event_id = core::StopEventId(21U);
  status.request_key = core::TestRequestKey("stop.status");
  status.requested = core::StopKind::kHalt;
  status.applied = core::StopKind::kHalt;
  status.covered = {core::StopEventCoveredOutcomeDTO{
      core::OperationNumber(6U), 1U, true, false, core::StopPhase::kStopping,
      std::nullopt, false, false, std::nullopt}};
  status.accepted_at_ns = 100;
  auto value = StopEventStatusValue(status);
  EXPECT_EQ(MemberNames(value),
            Sorted({"schema_version", "event_id", "request_key", "requested", "applied",
                    "widen_reason", "state", "result", "covered", "accepted_at_ns",
                    "completed_at_ns"}));
  EXPECT_EQ(value.find_object_member("schema_version")->as_string(),
            "manyforge.execution_stop_status.v2");
  EXPECT_EQ(value.find_object_member("event_id")->as_number(), 21.0);
  EXPECT_EQ(value.find_object_member("request_key")->as_string(),
            core::RequestKeyText(core::TestRequestKey("stop.status")));
  EXPECT_EQ(value.find_object_member("requested")->as_string(), "halt");
  EXPECT_EQ(value.find_object_member("applied")->as_string(), "halt");
  EXPECT_EQ(value.find_object_member("widen_reason")->as_string(), "none");
  EXPECT_EQ(value.find_object_member("state")->as_string(), "pending");
  EXPECT_EQ(value.find_object_member("result")->as_string(), "pending");
  EXPECT_EQ(value.find_object_member("accepted_at_ns")->as_number(), 100.0);
  EXPECT_TRUE(value.find_object_member("completed_at_ns")->is_null());
  {
    const auto &covered = value.find_object_member("covered")->as_array();
    ASSERT_EQ(covered.size(), 1U);
    EXPECT_EQ(MemberNames(covered[0]),
              Sorted({"operation_id", "participant_ordinal", "started", "done", "phase",
                      "containing_stage_index", "escalated", "stop_path_collision",
                      "error_code"}));
    EXPECT_EQ(covered[0].find_object_member("phase")->as_string(), "stopping");
    EXPECT_FALSE(covered[0].find_object_member("done")->as_bool());
    EXPECT_TRUE(covered[0].find_object_member("containing_stage_index")->is_null());
    EXPECT_TRUE(covered[0].find_object_member("error_code")->is_null());
  }
  status.complete = true;
  status.result = core::StopEventResult::kStoppedEscalated;
  status.completed_at_ns = 250;
  status.covered.front() = core::StopEventCoveredOutcomeDTO{
      core::OperationNumber(6U), 1U, true, true, core::StopPhase::kStopped,
      std::uint8_t{1U}, true, true, std::string("runtime_collision")};
  value = StopEventStatusValue(status);
  EXPECT_EQ(value.find_object_member("state")->as_string(), "complete");
  EXPECT_EQ(value.find_object_member("result")->as_string(), "stopped_escalated");
  EXPECT_EQ(value.find_object_member("completed_at_ns")->as_number(), 250.0);
  const auto &covered = value.find_object_member("covered")->as_array().front();
  EXPECT_TRUE(covered.find_object_member("done")->as_bool());
  EXPECT_EQ(covered.find_object_member("phase")->as_string(), "stopped");
  EXPECT_EQ(covered.find_object_member("containing_stage_index")->as_number(), 1.0);
  EXPECT_TRUE(covered.find_object_member("escalated")->as_bool());
  EXPECT_TRUE(covered.find_object_member("stop_path_collision")->as_bool());
  EXPECT_EQ(covered.find_object_member("error_code")->as_string(), "runtime_collision");
}

TEST(StopResponseCodec, CellStopActionNullAndCompleted) {
  const auto names = Sorted({"available", "last_event_id", "last_completed",
                             "unavailable_reason"});
  auto value = CellStopActionValue(core::CellStopActionProjectionDTO{});
  EXPECT_EQ(MemberNames(value), names);
  EXPECT_TRUE(value.find_object_member("available")->as_bool());
  EXPECT_TRUE(value.find_object_member("last_event_id")->is_null());
  EXPECT_TRUE(value.find_object_member("last_completed")->is_null());
  EXPECT_TRUE(value.find_object_member("unavailable_reason")->is_null());
  // A Stop accepted but not yet complete: a new id, no completion.
  value = CellStopActionValue(core::CellStopActionProjectionDTO{core::StopEventId(4U), std::nullopt});
  EXPECT_EQ(value.find_object_member("last_event_id")->as_number(), 4.0);
  EXPECT_TRUE(value.find_object_member("last_completed")->is_null());
  value = CellStopActionValue(core::CellStopActionProjectionDTO{
      core::StopEventId(5U),
      core::CellStopLastCompletedDTO{core::StopEventId(4U), core::StopKind::kHalt,
                                     core::StopEventResult::kStoppedEscalated, true, 99}});
  EXPECT_EQ(MemberNames(value), names);
  EXPECT_EQ(value.find_object_member("last_event_id")->as_number(), 5.0);
  const auto &completed = *value.find_object_member("last_completed");
  EXPECT_EQ(MemberNames(completed),
            Sorted({"event_id", "applied", "result", "escalated", "completed_at_ns"}));
  EXPECT_EQ(completed.find_object_member("event_id")->as_number(), 4.0);
  EXPECT_EQ(completed.find_object_member("applied")->as_string(), "halt");
  EXPECT_EQ(completed.find_object_member("result")->as_string(), "stopped_escalated");
  EXPECT_TRUE(completed.find_object_member("escalated")->as_bool());
  EXPECT_EQ(completed.find_object_member("completed_at_ns")->as_number(), 99.0);
}

TEST(StopResponseCodec, ProgramRunHoldNullAndFilled) {
  EXPECT_TRUE(ProgramRunHoldValue(std::nullopt).is_null());
  core::ProgramRunHoldFactsDTO hold{core::ProgramRunNumber(3U), 7U, core::RunFrozenLevel::kNone,
                                    std::nullopt, false, std::nullopt};
  auto value = ProgramRunHoldValue(hold);
  EXPECT_EQ(MemberNames(value),
            Sorted({"program_run_id", "hold_revision", "frozen", "frozen_by", "all_groups_held",
                    "held_by_rule", "frozen_kind", "frozen_source_label"}));
  EXPECT_TRUE(value.find_object_member("frozen_kind")->is_null());
  EXPECT_TRUE(value.find_object_member("frozen_source_label")->is_null());
  EXPECT_EQ(value.find_object_member("program_run_id")->as_number(), 3.0);
  EXPECT_EQ(value.find_object_member("hold_revision")->as_number(), 7.0);
  EXPECT_EQ(value.find_object_member("frozen")->as_string(), "none");
  EXPECT_TRUE(value.find_object_member("frozen_by")->is_null());
  EXPECT_FALSE(value.find_object_member("all_groups_held")->as_bool());
  EXPECT_TRUE(value.find_object_member("held_by_rule")->is_null());
  hold.frozen = core::RunFrozenLevel::kHalt;
  hold.frozen_by = core::StopEventId(11U);
  hold.all_groups_held = true;
  hold.held_by_rule = core::InterruptPolicyId("rule.door");
  hold.frozen_kind = core::StopKind::kStopFailedHalt;
  hold.frozen_source_label = "kernel.stop_failed";
  value = ProgramRunHoldValue(hold);
  EXPECT_EQ(value.find_object_member("frozen_kind")->as_string(), "stop_failed_halt");
  EXPECT_EQ(value.find_object_member("frozen_source_label")->as_string(), "kernel.stop_failed");
  EXPECT_EQ(value.find_object_member("frozen")->as_string(), "halt");
  EXPECT_EQ(value.find_object_member("frozen_by")->as_number(), 11.0);
  EXPECT_TRUE(value.find_object_member("all_groups_held")->as_bool());
  EXPECT_EQ(value.find_object_member("held_by_rule")->as_string(), "rule.door");
}

// OD-205 (b): the stream's operations table: the alarm facts only, no
// progress and no times; an incomplete entry carries no facts.
TEST(StopResponseCodec, OperationTableCarriesTheAlarmFactsAndNamesTheRuntimeMonitorStop) {
  core::OperationTableDTO table;
  table.change = 9U;
  table.faulted_total = 3U;
  EXPECT_EQ(MemberNames(OperationTableValue(table)),
            Sorted({"change", "faulted", "faulted_total", "resident", "resident_total"}));
  core::OperationTableRuntimeMonitorStopDTO monitor{};
  const std::string_view reason = "runtime_collision_fault";
  const std::string_view label = "kernel.runtime_collision";
  std::copy(reason.begin(), reason.end(), monitor.reason.bytes.begin());
  monitor.reason.length = static_cast<std::uint16_t>(reason.size());
  std::copy(label.begin(), label.end(), monitor.source_label.bytes.begin());
  monitor.source_label.length = static_cast<std::uint16_t>(label.size());
  monitor.request_key = core::TestRequestKey("request.collision");
  core::OperationTableEntryDTO stopped{};
  stopped.operation_id = core::OperationNumber(4U);
  stopped.operation_revision = core::OperationRevision(2U);
  stopped.state = core::OperationStateKind::kInterventionRequired;
  stopped.stop_event_id = core::StopEventId(6U);
  stopped.pending_control = core::OperationControlKind::kReset;
  stopped.runtime_monitor_stop = monitor;
  stopped.program_binding_id = "program-binding.test";
  stopped.program_run_id = core::ProgramRunNumber(1U);
  stopped.resources = {core::ResourceId("robot.arm")};
  stopped.resource_total = 1U;
  stopped.terminal_error = core::OperationTableErrorDTO{
      "execution_aborted", std::string("runtime_collision_fault"), "stopped", true};
  stopped.participants.push_back(core::OperationTableParticipantDTO{
      core::OperationStateKind::kInterventionRequired, true, true,
      std::string("runtime_collision_fault"),
      core::OperationTableTerminalFactDTO{core::ParticipantExecutionFactKind::kLocalFailure,
                                          core::ParticipantEffectKnowledge::kEffectSettled,
                                          core::OperationTableFactError::kSameAsTerminalError}});
  table.faulted.push_back(stopped);
  core::OperationTableEntryDTO incomplete{};
  incomplete.operation_id = core::OperationNumber(5U);
  incomplete.operation_revision = core::OperationRevision(1U);
  incomplete.state = core::OperationStateKind::kFailed;
  incomplete.complete = false;
  table.faulted.push_back(incomplete);
  const auto value = OperationTableValue(table);
  EXPECT_EQ(value.find_object_member("change")->as_number(), 9.0);
  EXPECT_EQ(value.find_object_member("faulted_total")->as_number(), 3.0);
  const auto &faulted = value.find_object_member("faulted")->as_array();
  ASSERT_EQ(faulted.size(), 2U);
  EXPECT_EQ(MemberNames(faulted[0]),
            Sorted({"complete", "operation_id", "operation_revision", "participants",
                    "pending_control", "program_binding_id", "program_run_id",
                    "resource_total", "resources", "runtime_monitor_stop", "state",
                    "stop_event_id", "terminal_error"}));
  EXPECT_EQ(faulted[0].find_object_member("state")->as_string(), "intervention_required");
  EXPECT_EQ(faulted[0].find_object_member("pending_control")->as_string(), "reset");
  const auto &stop = *faulted[0].find_object_member("runtime_monitor_stop");
  EXPECT_EQ(stop.find_object_member("reason")->as_string(), "runtime_collision_fault");
  EXPECT_EQ(stop.find_object_member("source_label")->as_string(), "kernel.runtime_collision");
  EXPECT_EQ(stop.find_object_member("request_key")->as_string(),
            core::RequestKeyText(core::TestRequestKey("request.collision")));
  const auto &error = *faulted[0].find_object_member("terminal_error");
  EXPECT_EQ(MemberNames(error), Sorted({"code", "message", "message_truncated", "reason"}));
  EXPECT_TRUE(error.find_object_member("message_truncated")->as_bool());
  const auto &participant = faulted[0].find_object_member("participants")->as_array().front();
  EXPECT_EQ(MemberNames(participant),
            Sorted({"execution_scope", "state", "terminal_error", "terminal_fact"}));
  EXPECT_EQ(participant.find_object_member("terminal_fact")
                ->find_object_member("error")->as_string(), "same");
  EXPECT_EQ(MemberNames(faulted[1]),
            Sorted({"complete", "operation_id", "operation_revision", "pending_control",
                    "runtime_monitor_stop", "state", "stop_event_id"}));
  EXPECT_FALSE(faulted[1].find_object_member("complete")->as_bool());
}

TEST(StopResponseCodec, StreamStateCarriesHoldFactsNotTheRunFence) {
  Value::Object empty;
  AppendStopStreamFacts(empty, core::StopStreamFactsDTO{});
  const Value empty_value(std::move(empty));
  EXPECT_EQ(MemberNames(empty_value),
            Sorted({"program_run_hold", "last_stop_event_id", "stop_progress_sequence"}));
  EXPECT_TRUE(empty_value.find_object_member("program_run_hold")->is_null());
  EXPECT_TRUE(empty_value.find_object_member("last_stop_event_id")->is_null());
  EXPECT_EQ(empty_value.find_object_member("stop_progress_sequence")->as_number(), 0.0);
  EXPECT_EQ(empty_value.find_object_member("program_run_fence"), nullptr);

  const core::ProgramRunHoldFactsDTO hold{core::ProgramRunNumber(3U), 7U,
                                          core::RunFrozenLevel::kRunnerLoss,
                                          core::StopEventId(13U), false, std::nullopt};
  Value::Object state{{"existing", Value(true)}};
  AppendStopStreamFacts(state, core::StopStreamFactsDTO{hold, core::StopEventId(14U), 5U});
  const Value value(std::move(state));
  EXPECT_EQ(value.as_object().size(), 4U);
  EXPECT_TRUE(value.find_object_member("existing")->as_bool());
  EXPECT_EQ(*value.find_object_member("program_run_hold"), ProgramRunHoldValue(hold));
  EXPECT_EQ(value.find_object_member("last_stop_event_id")->as_number(), 14.0);
  EXPECT_EQ(value.find_object_member("stop_progress_sequence")->as_number(), 5.0);
}

TEST(StopResponseCodec, ScopeReadCarriesHoldRevisionAndFrozenLevel) {
  core::ProgramExecutionScopeStateDTO scope{
      core::CellAdmissionClaimGeneration(1U), core::ProgramRunNumber(2U), 3U, {}, {}};
  scope.held_nodes = {{0U, core::ProgramNodeHoldStateDTO::kRunning},
                      {1U, core::ProgramNodeHoldStateDTO::kReady},
                      {2U, core::ProgramNodeHoldStateDTO::kHeld},
                      {3U, core::ProgramNodeHoldStateDTO::kStopping},
                      {4U, core::ProgramNodeHoldStateDTO::kFaulted}};
  auto value = ProgramExecutionScopeValue(scope);
  EXPECT_EQ(MemberNames(value),
            Sorted({"schema_version", "claim_generation", "program_run_id", "hold_revision",
                    "ready_node_indices", "held_node_indices", "stopping_node_indices",
                    "faulted_node_indices", "selection_release_block_reason", "stopped_by",
                    "rule_id", "program_held_nodes", "frozen", "frozen_by",
                    "all_groups_held"}));
  for (const auto *removed : {"stop_id", "stop_generation", "selection_revision",
                              "request_key", "releasable", "ended"})
    EXPECT_EQ(value.find_object_member(removed), nullptr) << removed;
  EXPECT_EQ(value.find_object_member("schema_version")->as_string(),
            "manyforge.program_execution_scope.v4");
  EXPECT_EQ(value.find_object_member("claim_generation")->as_number(), 1.0);
  EXPECT_EQ(value.find_object_member("program_run_id")->as_number(), 2.0);
  EXPECT_EQ(value.find_object_member("hold_revision")->as_number(), 3.0);
  const auto indices = [&](const char *name) {
    std::vector<double> result;
    for (const auto &index : value.find_object_member(name)->as_array())
      result.push_back(index.as_number());
    return result;
  };
  EXPECT_EQ(indices("ready_node_indices"), std::vector<double>{1.0});
  EXPECT_EQ(indices("held_node_indices"), std::vector<double>{2.0});
  EXPECT_EQ(indices("stopping_node_indices"), std::vector<double>{3.0});
  EXPECT_EQ(indices("faulted_node_indices"), std::vector<double>{4.0});
  EXPECT_TRUE(value.find_object_member("selection_release_block_reason")->is_null());
  EXPECT_TRUE(value.find_object_member("stopped_by")->is_null());
  EXPECT_TRUE(value.find_object_member("rule_id")->is_null());
  EXPECT_TRUE(value.find_object_member("program_held_nodes")->as_array().empty());
  EXPECT_EQ(value.find_object_member("frozen")->as_string(), "none");
  EXPECT_TRUE(value.find_object_member("frozen_by")->is_null());
  EXPECT_FALSE(value.find_object_member("all_groups_held")->as_bool());

  scope.selection_release_block_reason = core::ProgramSelectionReleaseBlockReasonDTO::kStopping;
  scope.stopped_by_rule_id = "rule.door";
  scope.frozen_by = core::StopEventId(17U);
  scope.all_groups_held = true;
  for (const auto &[level, token] :
       {std::pair{core::RunFrozenLevel::kHalt, "halt"},
        std::pair{core::RunFrozenLevel::kRunnerLoss, "runner_loss"},
        std::pair{core::RunFrozenLevel::kProgramEnd, "program_end"}}) {
    SCOPED_TRACE(token);
    scope.frozen = level;
    value = ProgramExecutionScopeValue(scope);
    EXPECT_EQ(value.find_object_member("frozen")->as_string(), token);
    EXPECT_EQ(value.find_object_member("frozen_by")->as_number(), 17.0);
    EXPECT_TRUE(value.find_object_member("all_groups_held")->as_bool());
    EXPECT_EQ(value.find_object_member("selection_release_block_reason")->as_string(),
              "stopping");
    EXPECT_EQ(value.find_object_member("stopped_by")->as_string(), "rule");
    EXPECT_EQ(value.find_object_member("rule_id")->as_string(), "rule.door");
  }

  // OD-342: the held nodes a Program node's own Stop holds, as
  // [held node index, requesting node index] pairs.
  scope.program_held_nodes = {{2U, 5U}, {3U, 5U}};
  value = ProgramExecutionScopeValue(scope);
  const auto &pairs = value.find_object_member("program_held_nodes")->as_array();
  ASSERT_EQ(pairs.size(), 2U);
  EXPECT_EQ(pairs[0].as_array()[0].as_number(), 2.0);
  EXPECT_EQ(pairs[0].as_array()[1].as_number(), 5.0);
  EXPECT_EQ(pairs[1].as_array()[0].as_number(), 3.0);
}

TEST(StopResponseCodec, ParticipantStopObservation) {
  core::ParticipantStopObservationDTO observation;
  observation.phase = core::StopPhase::kStopping;
  observation.attempt = 1U;
  observation.stop_event_id = core::StopEventId(30U);
  observation.mode = core::StopMode::kInterrupt;
  observation.attribution_class = core::StopAttributionClass::kRule;
  auto value = ParticipantStopObservationValue(observation);
  EXPECT_EQ(MemberNames(value),
            Sorted({"phase", "attempt", "stop_event_id", "mode", "attribution_class",
                    "plan_digest", "containing_stage_id", "containing_stage_index",
                    "escalated"}));
  EXPECT_EQ(value.find_object_member("phase")->as_string(), "stopping");
  EXPECT_EQ(value.find_object_member("attempt")->as_number(), 1.0);
  EXPECT_EQ(value.find_object_member("stop_event_id")->as_number(), 30.0);
  EXPECT_EQ(value.find_object_member("mode")->as_string(), "interrupt");
  EXPECT_EQ(value.find_object_member("attribution_class")->as_string(), "rule");
  EXPECT_TRUE(value.find_object_member("plan_digest")->is_null());
  EXPECT_TRUE(value.find_object_member("containing_stage_id")->is_null());
  EXPECT_TRUE(value.find_object_member("containing_stage_index")->is_null());
  EXPECT_FALSE(value.find_object_member("escalated")->as_bool());
  observation.phase = core::StopPhase::kStopped;
  observation.mode = core::StopMode::kSpringReturn;
  observation.attribution_class = core::StopAttributionClass::kOperator;
  observation.plan_digest = core::ContentDigest("plan.digest");
  observation.containing_stage_id = core::StopEscalationStageId("software_hard");
  observation.containing_stage_index = std::uint8_t{1U};
  observation.escalated = true;
  value = ParticipantStopObservationValue(observation);
  EXPECT_EQ(value.find_object_member("phase")->as_string(), "stopped");
  EXPECT_EQ(value.find_object_member("mode")->as_string(), "spring_return");
  EXPECT_EQ(value.find_object_member("attribution_class")->as_string(), "operator");
  EXPECT_EQ(value.find_object_member("plan_digest")->as_string(), "plan.digest");
  EXPECT_EQ(value.find_object_member("containing_stage_id")->as_string(), "software_hard");
  EXPECT_EQ(value.find_object_member("containing_stage_index")->as_number(), 1.0);
  EXPECT_TRUE(value.find_object_member("escalated")->as_bool());
}

}  // namespace
}  // namespace manyforge::roboplan_backend::host_detail
