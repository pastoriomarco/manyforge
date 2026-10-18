#include "execution_host_commands.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <limits>
#include <manyforge_content_identity/content_identity.hpp>

namespace manyforge::roboplan_backend::host_detail {
namespace {
using Value = content_identity::CanonicalValue;

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
} // namespace
} // namespace manyforge::roboplan_backend::host_detail


namespace manyforge::roboplan_backend::host_detail {
namespace {
Value MockStartRow(std::string target, std::string resource,
                   Value change = Value(nullptr)) {
  return Value(Value::Object{
      {"signal", Value(Value::Object{{"target_role", Value(std::move(target))},
                                      {"resource_role", Value(std::move(resource))}})},
      {"start_value", Value(false)}, {"change", std::move(change)}});
}
Value MockStartRequest(Value::Array rows = {MockStartRow("arm", "ready")}) {
  return Value(Value::Object{
      {"schema_version", Value("manyforge.mock_io_node_start.v1")},
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
           Replace(MockStartRequest(), "schema_version", Value("manyforge.mock_io_node_start.v2")),
           Replace(MockStartRequest(), "previous_node_attempt_id", Value("old.attempt")),
           Replace(MockStartRequest(), "source_incarnation", Value("caller-authority")),
           Replace(MockStartRequest(), "node_attempt_id", Value("")),
           MockStartRequest({}),
           MockStartRequest({MockStartRow("arm", "ready"), MockStartRow("arm", "ready")}),
           MockStartRequest({MockStartRow("z", "ready"), MockStartRow("a", "ready")}),
           MockStartRequest({Replace(MockStartRow("arm", "ready"), "catalog_generation", Value(1.0))}),
           MockStartRequest({Replace(MockStartRow("arm", "ready"), "signal",
               Value(Value::Object{{"target_role", Value("arm")}, {"resource_role", Value("ready")},
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
      Replace(MockEndRequest(), "schema_version", Value("manyforge.mock_io_node_start.v1")),
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
}  // namespace
}  // namespace manyforge::roboplan_backend::host_detail
