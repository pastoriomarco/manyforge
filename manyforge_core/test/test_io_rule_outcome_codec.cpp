#include <gtest/gtest.h>

#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/io_rule_outcome_codec.hpp>

#include "io_rule_outcome_fixture.hpp"

namespace manyforge::core {
namespace {
TEST(IoRuleOutcomeCodecTest, PreservesFullWidthUsedValuesAndIndependentStopFacts) {
  auto state = rule_codec_test::State();
  const auto& outcomes = state.participants.front().io_rule_outcomes;
  const auto encoded = EncodeIoRuleOutcomes(outcomes);
  ASSERT_TRUE(encoded.ok()) << encoded.error().message;
  EXPECT_NE(encoded.value().find("18446744073709551615"), std::string::npos);
  EXPECT_NE(encoded.value().find("-9223372036854775808"), std::string::npos);
  const auto decoded = DecodeIoRuleOutcomes(encoded.value());
  ASSERT_TRUE(decoded.ok()) << decoded.error().message;
  EXPECT_EQ(decoded.value(), outcomes);
  const auto& actual = decoded.value().front();
  EXPECT_EQ(actual.stop_result, StopCertificateResultDTO::kSucceededEscalated);
  EXPECT_EQ(actual.terminal_cause, MotionExecutionTerminalCause::kSignalTrip);
  EXPECT_EQ(actual.evidence.trip->blackboard->values.size(), 5U);
  EXPECT_EQ(std::get<std::string>(*actual.evidence.trip->blackboard->values.back()->value).size(),
            5000U);
  // Decoder owns only the compact used values, with deep equality and no alias
  // into the producer's full source snapshot or original entries.
  EXPECT_NE(actual.evidence.trip->blackboard->values.front().get(),
            outcomes.front().evidence.trip->blackboard->values.front().get());
  EXPECT_EQ(EncodeIoRuleOutcomes(decoded.value()).value(), encoded.value());
}
TEST(IoRuleOutcomeCodecTest, BaselineTripDoesNotFabricateCausalAcquisition) {
  auto state = rule_codec_test::State();
  auto& outcome = state.participants.front().io_rule_outcomes.front();
  outcome.evidence.trip->triggering_acquisition.reset();
  outcome.arbitration_disposition.reset();
  auto bytes = EncodeIoRuleOutcomes(state.participants.front().io_rule_outcomes);
  ASSERT_TRUE(bytes.ok());
  EXPECT_EQ(bytes.value().find("arbitration_disposition"), std::string::npos);
  auto decoded = DecodeIoRuleOutcomes(bytes.value());
  ASSERT_TRUE(decoded.ok());
  EXPECT_FALSE(decoded.value().front().evidence.trip->triggering_acquisition);
}
TEST(IoRuleOutcomeCodecTest, CausalSignalAndSupportingBlackboardRemainDistinctWithLossFacts) {
  auto state = rule_codec_test::State();
  auto& outcome = state.participants.front().io_rule_outcomes.front();
  auto& trip = *outcome.evidence.trip;
  const InterruptSourceId physical("door.source");
  const IoSourceIncarnation epoch("door.epoch");
  const IoSourceCursorDTO cursor{outcome.registration.owner_epoch, UINT64_MAX, physical,
                                 UINT64_MAX};
  trip.source_cursors.push_back(cursor);
  trip.matched_samples.push_back({{{ControllableComponentId("robot.1"), "door"},
                                   physical,
                                   epoch,
                                   123U,
                                   IoSampleValidity::kValid,
                                   false,
                                   std::nullopt},
                                  INT64_MAX});
  trip.triggering_acquisition = IoRuleAcquisitionKeyDTO{physical, epoch, 123U, cursor};
  IoIngressLossBarrierDTO barrier{physical, epoch, INT64_MAX - 1, INT64_MAX, 123U,      true,
                                  true,     false, true,          true,      UINT64_MAX};
  outcome.evidence.source_losses.push_back(
      {outcome.registration, physical, epoch, cursor, IoRuleSourceFailureKind::kContinuityLoss,
       IoSourceLossDTO{IoSourceLossReason::kIngressOverflow, 123U, 124U, 130U,
                       ErrorDTO{"overflow", "actual ingress rejected", {{"transport", "ros"}}},
                       barrier},
       std::nullopt, INT64_MAX});
  auto encoded = EncodeIoRuleOutcomes(state.participants.front().io_rule_outcomes);
  ASSERT_TRUE(encoded.ok()) << encoded.error().message;
  EXPECT_NE(encoded.value().find("\"controllable_component_id\":\"robot.1\""),
            std::string::npos);
  EXPECT_EQ(encoded.value().find("\"component_id\""), std::string::npos);
  auto old_key = encoded.value();
  const auto key_at = old_key.find("\"controllable_component_id\":\"robot.1\"");
  ASSERT_NE(key_at, std::string::npos);
  old_key.replace(key_at, std::string("\"controllable_component_id\"").size(),
                  "\"component_id\"");
  EXPECT_FALSE(DecodeIoRuleOutcomes(old_key).ok());
  const auto decoded = DecodeIoRuleOutcomes(encoded.value());
  ASSERT_TRUE(decoded.ok());
  EXPECT_EQ(decoded.value(), state.participants.front().io_rule_outcomes);
  EXPECT_EQ(decoded.value().front().evidence.trip->triggering_acquisition->source_id, physical);
  EXPECT_EQ(decoded.value().front().evidence.trip->blackboard->message_sequence, UINT64_MAX);
  EXPECT_EQ(decoded.value()
                .front()
                .evidence.source_losses.back()
                .source_loss->ingress_barrier->middleware_reported_lost_messages,
            UINT64_MAX);
}
TEST(IoRuleOutcomeCodecTest, RejectsRelabelingNullEntriesAndContradictoryFrames) {
  const auto original = rule_codec_test::State();
  auto state = original;
  state.participants.front().command_generation = ControllerGoalGeneration(2U);
  EXPECT_FALSE(ValidateIoRuleOutcomeParticipant(state.operation_id, state.operation_revision,
                                                state.participants.front())
                   .ok());
  state = original;
  state.participants.front()
      .io_rule_outcomes.front()
      .evidence.trip->blackboard->values.front()
      .reset();
  EXPECT_FALSE(EncodeIoRuleOutcomes(state.participants.front().io_rule_outcomes).ok());
  state = original;
  state.participants.front().io_rule_outcomes.front().evidence.trip->blackboard->message_sequence -=
      1U;
  EXPECT_FALSE(EncodeIoRuleOutcomes(state.participants.front().io_rule_outcomes).ok());
  state = original;
  state.participants.front()
      .io_rule_outcomes.front()
      .evidence.source_losses.front()
      .blackboard->source_id = InterruptSourceId("wrong");
  EXPECT_FALSE(EncodeIoRuleOutcomes(state.participants.front().io_rule_outcomes).ok());
  state = original;
  auto& values =
      state.participants.front().io_rule_outcomes.front().evidence.trip->blackboard->values;
  values.push_back(values.back());
  EXPECT_FALSE(EncodeIoRuleOutcomes(state.participants.front().io_rule_outcomes).ok());
}
TEST(IoRuleOutcomeCodecTest, HistoricalOriginPrecedesControlRevisionWithoutRelabeling) {
  const RequestOwnerEpoch owner("owner.1");
  for (const auto origin : {1U, 2U}) {
    auto state = rule_codec_test::State();
    state.operation_revision = OperationRevision(origin);
    state.participants.front().io_rule_outcomes = {rule_codec_test::Outcome(state, owner)};
    const auto original = state.participants.front().io_rule_outcomes;
    const auto before = EncodeIoRuleOutcomes(original);
    ASSERT_TRUE(before.ok());
    EXPECT_TRUE(ValidateIoRuleOutcomeParticipant(
                    state.operation_id, state.operation_revision, state.participants.front()).ok());
    // ACK after the original execution, and ACK after an execution admitted at
    // revision 2, both retain their own positive immutable origin revision.
    state.operation_revision = OperationRevision(origin + 1U);
    EXPECT_TRUE(ValidateIoRuleOutcomeParticipant(
                    state.operation_id, state.operation_revision, state.participants.front()).ok());
    const auto& participant = state.participants.front();
    EXPECT_TRUE(ValidateIoRuleOutcomeParticipant(
                    state.operation_id, state.operation_revision, participant.participant_id,
                    participant.controllable_component_id, participant.command_target_id,
                    participant.command_id, participant.command_generation,
                    participant.io_rule_outcomes).ok());
    const auto after = EncodeIoRuleOutcomes(state.participants.front().io_rule_outcomes);
    ASSERT_TRUE(after.ok());
    EXPECT_EQ(after.value(), before.value());
    const auto decoded = DecodeIoRuleOutcomes(after.value());
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(decoded.value(), original);
    const auto& scope = std::get<IoOperationRuleScopeDTO>(decoded.value().front().registration.scope);
    EXPECT_EQ(scope.operation_revision, OperationRevision(origin));
    EXPECT_EQ(scope.controller_goal_id, state.participants.front().command_id);
    EXPECT_EQ(scope.controller_goal_generation,
              state.participants.front().command_generation);
  }
}
TEST(IoRuleOutcomeCodecTest, HistoricalContainmentRejectsZeroFutureAndForeignIdentities) {
  const auto original = rule_codec_test::State();
  const auto valid = [&](const auto& state) {
    return ValidateIoRuleOutcomeParticipant(
        state.operation_id, state.operation_revision, state.participants.front()).ok();
  };
  auto state = original;
  state.operation_revision = OperationRevision(2U);
  ASSERT_TRUE(valid(state));
  const auto historical = state;
  for (const auto origin : {0U, 3U}) {
    state = historical;
    std::get<IoOperationRuleScopeDTO>(state.participants.front().io_rule_outcomes.front()
                                         .registration.scope).operation_revision =
        OperationRevision(origin);
    EXPECT_FALSE(valid(state));
  }
  state = historical;
  state.operation_id = OperationId("foreign.operation");
  EXPECT_FALSE(valid(state));
  state = historical;
  state.participants.front().participant_id = OperationParticipantId("foreign.participant");
  EXPECT_FALSE(valid(state));
  state = historical;
  state.participants.front().controllable_component_id = ControllableComponentId("foreign.robot");
  EXPECT_FALSE(valid(state));
  state = historical;
  state.participants.front().command_target_id = CommandTargetId("foreign.target");
  EXPECT_FALSE(valid(state));
  state = historical;
  state.participants.front().command_id = ControllerGoalId("foreign.goal");
  EXPECT_FALSE(valid(state));
  state = historical;
  state.participants.front().command_generation = ControllerGoalGeneration(2U);
  EXPECT_FALSE(valid(state));
}
TEST(IoRuleOutcomeCodecTest, ClosedDecoderRejectsCoercionOverflowAndExtraFields) {
  auto state = rule_codec_test::State();
  auto encoded = EncodeIoRuleOutcomes(state.participants.front().io_rule_outcomes);
  ASSERT_TRUE(encoded.ok());
  for (const auto& replacement :
       {std::string("9223372036854775808"), std::string("+1"), std::string("01")}) {
    auto bytes = encoded.value();
    const auto p = bytes.find("9223372036854775807", bytes.find("integer_high"));
    ASSERT_NE(p, std::string::npos);
    bytes.replace(p, 19U, replacement);
    EXPECT_FALSE(DecodeIoRuleOutcomes(bytes).ok());
  }
  auto bytes = encoded.value();
  bytes.insert(bytes.find('{') + 1U, "\"caller_validity\":true,");
  EXPECT_FALSE(DecodeIoRuleOutcomes(bytes).ok());
  bytes = encoded.value();
  const auto p = bytes.find("\"-9223372036854775808\"");
  ASSERT_NE(p, std::string::npos);
  bytes.replace(p, 22U, "-9223372036854775808");
  EXPECT_FALSE(DecodeIoRuleOutcomes(bytes).ok());
}
}  // namespace
}  // namespace manyforge::core
