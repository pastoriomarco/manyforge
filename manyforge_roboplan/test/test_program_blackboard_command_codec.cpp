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
      R"({"api_version":"manyforge.execution_command.v3","command":"component_recovery_preflight","request":{"component_id":"arm"}})");
  EXPECT_EQ(preflight.kind, CommandKind::kComponentRecoveryPreflight);
  EXPECT_EQ(preflight.component_id, std::optional<std::string>("arm"));
  const auto confirmed = DecodeCommand(
      R"({"api_version":"manyforge.execution_command.v3","command":"component_action","request":{"action":"enable","component_id":"arm","recovery_preflight":{"confirmed":[{"cause_code":"fault_vendor","component_id":"arm","eligibility":"recover","stop_code":null,"stop_vendor_code":null,"vendor_code":"C31"}],"snapshot_id":"snapshot"}}})");
  ASSERT_TRUE(confirmed.recovery_preflight);
  ASSERT_EQ(confirmed.recovery_preflight->confirmed.size(), 1U);
  EXPECT_EQ(confirmed.recovery_preflight->confirmed[0].vendor_code, std::optional<std::string>("C31"));
  EXPECT_THROW(DecodeCommand(
      R"({"api_version":"manyforge.execution_command.v3","command":"component_action","request":{"action":"enable","component_id":"arm","recovery_preflight":{"confirmed":[{"cause_code":null,"component_id":"arm","eligibility":"ready","vendor_code":null}],"snapshot_id":"snapshot"}}})"), std::exception);
}
using Value = content_identity::CanonicalValue;

TEST(ToolPayloadCommandCodec, RemovedBridgeCommandIsRejected) {
  const auto command = content_identity::canonical_json(Value(Value::Object{
      {"api_version", Value(std::string(kCommandApiVersion))},
      {"command", Value("apply_tool_payload")}}));
  EXPECT_THROW(DecodeCommand(command), std::exception);
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
      {"schema_version", Value("manyforge.program_bbkey_publication.v1")},
      {"owner_epoch", Value("actual-owner-required-by-Core")},
      {"program_runner_owner_epoch", Value("actual-runner-required-by-Core")},
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
      {"schema_version", Value("manyforge.mock_io_node_start.v2")},
      {"owner_epoch", Value("kernel.epoch.1")},
      {"program_binding_id", Value("binding.1")},
      {"program_runner_owner_epoch", Value("runner.epoch.1")},
      {"claim_generation", Value(1.0)}, {"program_run_id", Value("run.1")},
      {"node_instance_id", Value("wait")}, {"node_attempt_id", Value("wait.attempt.1")},
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
  EXPECT_EQ(request.scope.program_run_id.value(), "run.1");
  EXPECT_EQ(request.scope.node.node_attempt_id, "wait.attempt.1");
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
           Replace(MockStartRequest(), "schema_version", Value("manyforge.mock_io_node_start.v1")),
           Replace(MockStartRequest(), "previous_node_attempt_id", Value("old.attempt")),
           Replace(MockStartRequest(), "source_incarnation", Value("caller-authority")),
           Replace(MockStartRequest(), "node_attempt_id", Value("")),
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
  return Replace(Value(std::move(rows)), "schema_version", Value("manyforge.mock_io_node_end.v1"));
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
  EXPECT_EQ(scope.node.node_attempt_id, "wait.attempt.1");
  EXPECT_EQ(scope.program_run_id.value(), "run.1");
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
      Replace(MockEndRequest(), "schema_version", Value("manyforge.mock_io_node_start.v2")),
      Replace(MockEndRequest(), "node_attempt_id", Value(""))})
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
      {"schema_version", Value("manyforge.mock_io_run_pause.v1")},
      {"owner_epoch", Value("kernel.epoch.1")}, {"program_binding_id", Value("binding.1")},
      {"program_runner_owner_epoch", Value("runner.epoch.1")},
      {"claim_generation", Value(1.0)}, {"program_run_id", Value("run.1")},
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
  EXPECT_EQ(running.mock_run_simulation_pause->scope.program_run_id.value(), "run.1");
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
      Replace(MockRunPauseRequest(), "schema_version", Value("manyforge.mock_io_run_pause.v2"))})
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
        {"api_version", Value("manyforge.execution_command.v3")},
        {"command", Value("read_volatile_motion_trace")},
        {"allowance_bytes", Value(allowance)},
        {"attribution", Value(Value::Object{
            {"transport", Value("internal")}, {"assurance", Value("unverified")},
            {"principal_id", Value(nullptr)}})}}));
  };
  const auto command = DecodeCommand(request(3145728.0));
  EXPECT_EQ(command.kind, CommandKind::kReadVolatileMotionTrace);
  EXPECT_EQ(command.trace_allowance_bytes, 3145728U);
  EXPECT_FALSE(IsSubmissionOrdered(command.kind));
  EXPECT_FALSE(ReadsManagerScene(command.kind));
  EXPECT_FALSE(IsStopLaneRead(command.kind));
  EXPECT_FALSE(PreemptsPlanning(command.kind));
  for (const double invalid : {0.0, -1.0, 0.5, 3145729.0})
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
      {"contact_policy_id", Value(nullptr)},
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
             Value(Value::Object{{"request_id", Value("request.endpoint")},
                                 {"request_owner_epoch", Value("owner")},
                                 {"source_label", Value("test")},
                                 {"correlation_id", Value(nullptr)}})},
            {"program_id", Value("program")},
            {"program_revision", Value("revision")},
            {"program_binding_id", Value("binding")},
            {"program_run_id", Value("run")},
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
  // Contact is explicit even for a joint destination. Spatial references
  // remain invalid; resolving this policy/object pair remains Core-owned.
  const Value runtime_object(Value::Object{
      {"runtime_object_id", Value("selected_object")},
      {"scene_resource_id", Value("probe_block")}});
  const auto withdrawal = Replace(
      Replace(payload, "contact_policy_id", Value("withdrawal.contact")),
      "runtime_object_bindings", Value(Value::Array{runtime_object}));
  const auto decoded_withdrawal = decode(withdrawal);
  const auto &joint_contact = std::get<core::MotionParticipantIntentDTO>(
      decoded_withdrawal.participants.front().payload);
  ASSERT_TRUE(joint_contact.contact_policy_id.has_value());
  EXPECT_EQ(joint_contact.contact_policy_id->value(), "withdrawal.contact");
  ASSERT_EQ(joint_contact.runtime_object_bindings.size(), 1U);
  EXPECT_EQ(joint_contact.runtime_object_bindings.front().runtime_object_id.value(),
            "selected_object");
  EXPECT_EQ(joint_contact.runtime_object_bindings.front().scene_resource_id.value(),
            "probe_block");
  EXPECT_THROW(decode(Replace(withdrawal, "contact_policy_id", Value(nullptr))),
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
      decode(Replace(withdrawal, "runtime_object_bindings",
                     Value(Value::Array{Replace(runtime_object, "extra", Value(true))}))),
      std::exception);
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
             Value(Value::Object{{"request_id", Value("request.set_tcp")},
                                 {"request_owner_epoch", Value("owner")},
                                 {"source_label", Value("test")},
                                 {"correlation_id", Value(nullptr)}})},
            {"program_id", Value("program")},
            {"program_revision", Value("revision")},
            {"program_binding_id", Value("binding")},
            {"program_run_id", Value("run")},
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
  const std::string owner =
      "kernel-owner-execution-host-096bf5ba514b8728699ff5c290ccd23f";
  const Value request(Value::Object{
      {"schema_version", Value("manyforge.mock_io_input_request.v2")},
      {"owner_epoch", Value(owner)},
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
    EXPECT_EQ(input.query.owner_epoch, core::RequestOwnerEpoch(owner));
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
      {"request_id", Value("recovery.codec.request")},
      {"action", Value("reset_cycle")},
      {"principal_id", Value("operator.codec")},
      {"expected_assessment_revision", Value(7.0)},
      {"expected_control_command_epoch", Value(3.0)},
      {"expected_execution_id", Value(42.0)},
      {"expected_stop_certificate_id", Value("certificate.codec")},
      {"expected_stop_owner_generation", Value(2.0)},
      {"reason", Value("codec validation only")}});
  const auto decode = [](Value selected) {
    return DecodeCommand(content_identity::canonical_json(Value(
        Value::Object{{"api_version", Value(std::string(kCommandApiVersion))},
                      {"command", Value("apply_operation_recovery")},
                      {"operation_id", Value("operation.codec")},
                      {"expected_operation_revision", Value(5.0)},
                      {"request", std::move(selected)}})));
  };
  const auto omitted = decode(request);
  ASSERT_TRUE(omitted.operation_recovery);
  EXPECT_EQ(omitted.kind, CommandKind::kApplyOperationRecovery);
  ASSERT_TRUE(omitted.operation_id);
  EXPECT_EQ(*omitted.operation_id, core::OperationId("operation.codec"));
  ASSERT_TRUE(omitted.expected_operation_revision);
  EXPECT_EQ(*omitted.expected_operation_revision, core::OperationRevision(5U));
  const auto &captured = *omitted.operation_recovery;
  EXPECT_EQ(captured.action, core::RecoveryAction::kResetCycle);
  EXPECT_EQ(captured.expected_assessment_revision, 7U);
  EXPECT_EQ(captured.expected_control_command_epoch, 3U);
  EXPECT_EQ(captured.expected_execution_id, 42U);
  EXPECT_EQ(captured.expected_stop_certificate_id, "certificate.codec");
  EXPECT_EQ(captured.expected_stop_owner_generation, 2U);
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
            Value("manyforge.kernel_recovery_assessment.v1"));
  const auto *eligibility = encoded.find_object_member("retry_eligibility");
  ASSERT_NE(eligibility, nullptr);
  EXPECT_EQ(*eligibility,
            Value(Value::Object{{"cause", Value("not_eligible")},
                                {"effect", Value("effect_uncertain")},
                                {"eligible", Value(false)},
                                {"reason", Value("not_retryable_state")}}));
  const core::KernelRecoveryActionResultDTO rejected{
      "recovery.codec.request", core::RecoveryAction::kResetCycle, false,
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

}  // namespace manyforge::roboplan_backend::host_detail
