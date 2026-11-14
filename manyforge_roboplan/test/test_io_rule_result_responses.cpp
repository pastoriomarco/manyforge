#include <manyforge_core/testing/request_key_fixture.hpp>
#include "execution_host_responses.hpp"
#include "io_rule_outcome_fixture.hpp"
#include <gtest/gtest.h>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/types/operation_member_id_json.hpp>
#include <manyforge_core/io_rule_outcome_codec.hpp>
#include <manyforge_core/types/typed_error.hpp>
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace manyforge::roboplan_backend::host_detail {
namespace {
TEST(ComponentActionResponseTest, CompositePhasesPreserveOrderAndUnknownCause) {
  core::ActionResult result;
  result.action = core::ComponentAction::kEnable;
  result.outcome = core::ActionOutcome::kUnknown;
  result.phases = {
      {"machine", core::ComponentAction::kRecover, core::ActionOutcome::kSucceeded, std::nullopt},
      {"arm", core::ComponentAction::kRecover, core::ActionOutcome::kUnknown,
       core::ActionError{"recovery_unknown", "answer missing", std::nullopt, std::nullopt}}};
  const auto value = ActionResultValue(result);
  const auto *phases = value.find_object_member("phases");
  ASSERT_NE(phases, nullptr);
  ASSERT_TRUE(phases->is_array());
  ASSERT_EQ(phases->as_array().size(), 2U);
  EXPECT_EQ(phases->as_array()[0].find_object_member("component_id")->as_string(), "machine");
  EXPECT_EQ(phases->as_array()[0].find_object_member("action")->as_string(), "recover");
  EXPECT_EQ(phases->as_array()[1].find_object_member("outcome")->as_string(), "unknown");
  ASSERT_NE(phases->as_array()[1].find_object_member("error"), nullptr);
  EXPECT_EQ(phases->as_array()[1].find_object_member("error")->find_object_member("code")->as_string(),
            "recovery_unknown");
  result.phases.clear();
  EXPECT_EQ(ActionResultValue(result).find_object_member("phases"), nullptr);
}

TEST(TypedErrorResponseTest, ProducerIsOnTopLevelRpcErrorOnly) {
  const core::ErrorDTO error{"stale_generation", "stale",
                             {{"reason", "expected_generation_stale"}}};
  const auto nested = ErrorValue(error);
  EXPECT_EQ(nested.find_object_member("producer"), nullptr);

  const auto response = ErrorResponse(error.code, error.message, error.details);
  const auto *wire_error = response.find_object_member("error");
  ASSERT_NE(wire_error, nullptr);
  const auto *producer = wire_error->find_object_member("producer");
  ASSERT_NE(producer, nullptr);
  EXPECT_EQ(producer->as_string(), "kernel");
  EXPECT_EQ(wire_error->find_object_member("code")->as_string(), error.code);
  EXPECT_EQ(wire_error->find_object_member("details")->as_object().size(), 1U);

  const core::TypedError typed("stale_generation", "stale",
                               {{"reason", "expected_generation_stale"}},
                               "component_manager");
  const auto refused = ExecutionCommandExceptionError(typed);
  EXPECT_EQ(refused.code, "execution_command_invalid");
  EXPECT_EQ(refused.message, "stale");
  EXPECT_EQ(refused.details.at("cause_code"), "stale_generation");
  EXPECT_EQ(refused.details.at("cause_producer"), "component_manager");
  const auto typed_response = ErrorResponse(
      refused.code, refused.message, refused.details);
  const auto *typed_error = typed_response.find_object_member("error");
  ASSERT_NE(typed_error, nullptr);
  const auto *typed_producer = typed_error->find_object_member("producer");
  ASSERT_NE(typed_producer, nullptr);
  EXPECT_EQ(typed_producer->as_string(), "kernel");
  EXPECT_EQ(typed_error->find_object_member("code")->as_string(),
            "execution_command_invalid");
}

// OD-213: the stream's operations table lists each resident operation with
// the `read` reply's members; its state leaves out the participants'
// `progress` and the `updated_at_ns` (they change at every progress sample),
// and a state that encodes to more than its bound is listed as null.
TEST(IoRuleResultResponseTest, OperationsTableListsResidentOperationsAsReadWithoutProgress) {
  const auto names = [](const CanonicalValue &value) {
    std::vector<std::string> result;
    for (const auto &[name, member] : value.as_object()) {
      static_cast<void>(member);
      result.push_back(name);
    }
    return result;
  };
  const auto without = [](std::vector<std::string> list, const std::string &name) {
    list.erase(std::remove(list.begin(), list.end(), name), list.end());
    return list;
  };
  const auto state = core::rule_codec_test::State();
  auto large = state;
  large.operation_id = core::OperationNumber(state.operation_id.value() + 1U);
  large.participants.front().io_rule_outcomes.clear();
  large.participants.front().terminal_error = core::ErrorDTO{
      "execution_aborted",
      std::string(core::kOperationTableMaximumResidentOperationBytes, 'x'), {}};
  const core::OperationControlAcceptedDTO control{
      core::TestRequestKey("request.table.reset"), core::RequestReplayStatusDTO::kAcceptedNew,
      large.operation_id, core::OperationRevision(1U), core::OperationControlKind::kReset,
      core::OperationRevision(2U), 5};
  core::OperationTableDTO table;
  table.change = 4U;
  table.resident.push_back(core::OperationReadProjectionDTO{state, true, std::nullopt, true});
  table.resident.push_back(core::OperationReadProjectionDTO{large, true, control, false});
  table.resident_total = 3U;

  const auto value = OperationTableValue(table);
  EXPECT_EQ(value.find_object_member("resident_total")->as_number(), 3.0);
  const auto &resident = value.find_object_member("resident")->as_array();
  ASSERT_EQ(resident.size(), 2U);
  EXPECT_EQ(names(resident[0]),
            (std::vector<std::string>{"control_pending", "last_control", "operation",
                                      "operation_id"}));
  EXPECT_EQ(resident[0].find_object_member("operation_id")->as_number(),
            static_cast<double>(state.operation_id.value()));
  EXPECT_TRUE(resident[0].find_object_member("control_pending")->as_bool());
  EXPECT_TRUE(resident[0].find_object_member("last_control")->is_null());
  const auto &listed = *resident[0].find_object_member("operation");
  EXPECT_EQ(listed, OperationTableStateValue(state));
  const auto read = OperationStateValue(state);
  EXPECT_EQ(names(listed), without(names(read), "updated_at_ns"));
  for (const auto &[name, member] : listed.as_object()) {
    if (name == "participants") continue;
    EXPECT_EQ(member, *read.find_object_member(name)) << name;
  }
  const auto &listed_participant = listed.find_object_member("participants")->as_array().front();
  const auto &read_participant = read.find_object_member("participants")->as_array().front();
  EXPECT_EQ(names(listed_participant), without(names(read_participant), "progress"));
  for (const auto &[name, member] : listed_participant.as_object())
    EXPECT_EQ(member, *read_participant.find_object_member(name)) << name;
  // Too large to list: the reader reads it.
  EXPECT_TRUE(resident[1].find_object_member("operation")->is_null());
  EXPECT_EQ(*resident[1].find_object_member("last_control"), ControlAcceptedValue(control));
  EXPECT_FALSE(resident[1].find_object_member("control_pending")->as_bool());
}

TEST(IoRuleResultResponseTest,
     PublicProjectionUsesSameClosedEvidenceAsDurableCodec) {
  const auto state = core::rule_codec_test::State();
  const auto response = OperationStateValue(state);
  const auto &participant =
      response.find_object_member("participants")->as_array().front();
  EXPECT_EQ(*participant.find_object_member("command_id"),
            core::ControllerGoalIdValue(state.participants.front().command_id));
  EXPECT_EQ(participant.find_object_member("command_generation"), nullptr);
  EXPECT_EQ(participant.find_object_member("controller_goal_id"), nullptr);
  EXPECT_EQ(participant.find_object_member("controller_goal_generation"), nullptr);
  const auto *evidence = participant.find_object_member("io_rule_outcomes");
  ASSERT_NE(evidence, nullptr);
  const auto decoded =
      core::DecodeIoRuleOutcomes(content_identity::canonical_json(*evidence));
  ASSERT_TRUE(decoded.ok());
  EXPECT_EQ(decoded.value(), state.participants.front().io_rule_outcomes);
  const auto &row = evidence->as_array().front();
  EXPECT_EQ(row.find_object_member("stop_result")->as_string(),
            "stopped_escalated");
  EXPECT_EQ(row.find_object_member("terminal_cause")->as_string(),
            "signal_trip");
  const auto &values = row.find_object_member("evidence")
                           ->find_object_member("trip")
                           ->find_object_member("blackboard")
                           ->find_object_member("values")
                           ->as_array();
  EXPECT_EQ(values.size(), 5U);
}
TEST(IoRuleResultResponseTest,
     AbsentExtensionKeepsLegacyShapeAndForeignGoalRefuses) {
  auto state = core::rule_codec_test::State();
  state.participants.front().io_rule_outcomes.clear();
  auto response = OperationStateValue(state);
  EXPECT_EQ(response.find_object_member("participants")
                ->as_array()
                .front()
                .find_object_member("io_rule_outcomes"),
            nullptr);
  state = core::rule_codec_test::State();
  state.participants.front().command_id =
      core::ControllerGoalId{core::OperationNumber(901U), 1U, 1U};
  EXPECT_THROW((void)OperationStateValue(state), std::exception);
}
TEST(OperationStateResponseTest, AbsentTakeoverKeepsLegacyShape) {
  const auto response = OperationStateValue(core::rule_codec_test::State());
  EXPECT_EQ(response.find_object_member("motion_takeover"), nullptr);
}

TEST(OperationStateResponseTest, TakeoverProjectsExactDecisionAndStopEvent) {
  auto state = core::rule_codec_test::State();
  state.motion_takeover = core::MotionTakeoverDecisionDTO{
      .incumbent_operation_id = core::OperationNumber(1001U),
      .path = core::MotionTakeoverPathDTO::kMovingReplacement,
      .cause = std::nullopt,
      .decided_at_ns = INT64_MAX,
      .elapsed_since_acceptance_ns = 9007199254740993LL,
      .preparation_budget_ns = 1500000000,
      .stop_event_id = std::nullopt,
      .planning_budget_ns = 1200000000};
  auto response = OperationStateValue(state);
  const auto *decision = response.find_object_member("motion_takeover");
  ASSERT_NE(decision, nullptr);
  EXPECT_EQ(decision->as_object().size(), 8U);
  EXPECT_EQ(decision->find_object_member("incumbent_operation_id")->as_number(),
            1001.0);
  EXPECT_EQ(decision->find_object_member("path")->as_string(),
            "moving_replacement");
  EXPECT_EQ(content_identity::canonical_json(
                *decision->find_object_member("cause")), "null");
  EXPECT_EQ(content_identity::canonical_json(
                *decision->find_object_member("stop_event_id")), "null");
  EXPECT_EQ(decision->find_object_member("stop_certificate"), nullptr);
  EXPECT_EQ(decision->find_object_member("decided_at_ns")->as_string(),
            "9223372036854775807");
  EXPECT_EQ(decision->find_object_member("elapsed_since_acceptance_ns")->as_string(),
            "9007199254740993");
  EXPECT_EQ(decision->find_object_member("preparation_budget_ns")->as_number(),
            1500000000.0);
  EXPECT_EQ(decision->find_object_member("planning_budget_ns")->as_number(),
            1200000000.0);

  const core::ErrorDTO cause{
      "motion_takeover_preparation_failed", "original planner failure",
      {{"planner_detail", "preserve exact cause"}}};
  state.motion_takeover->path = core::MotionTakeoverPathDTO::kSpringBackHold;
  state.motion_takeover->cause = cause;
  response = OperationStateValue(state);
  decision = response.find_object_member("motion_takeover");
  ASSERT_NE(decision, nullptr);
  EXPECT_TRUE(decision->find_object_member("stop_event_id")->is_null());
  EXPECT_EQ(content_identity::canonical_json(
                *decision->find_object_member("cause")),
            content_identity::canonical_json(ErrorValue(cause)));
  state.motion_takeover->stop_event_id = core::StopEventId(1000U);
  for (const auto &[path, spelling] :
       {std::pair{core::MotionTakeoverPathDTO::kSpringBackHold,
                  "spring_back_hold"},
        std::pair{core::MotionTakeoverPathDTO::kOrdinaryContainment,
                  "ordinary_containment"}}) {
    state.motion_takeover->path = path;
    response = OperationStateValue(state);
    decision = response.find_object_member("motion_takeover");
    ASSERT_NE(decision, nullptr);
    EXPECT_EQ(decision->as_object().size(), 8U);
    EXPECT_EQ(decision->find_object_member("path")->as_string(), spelling);
    EXPECT_EQ(content_identity::canonical_json(
                  *decision->find_object_member("cause")),
              content_identity::canonical_json(ErrorValue(cause)));
    EXPECT_EQ(decision->find_object_member("stop_event_id")->as_number(), 1000.0);
  }
}
// Stop state model: the operation carries the event that started its newest
// attempt and each participant its stop observation (null while never
// covered); the former active Stop id/generation members are gone.
TEST(OperationStateResponseTest, StopEventAndParticipantStopObservation) {
  auto state = core::rule_codec_test::State();
  auto response = OperationStateValue(state);
  ASSERT_NE(response.find_object_member("stop_event_id"), nullptr);
  EXPECT_TRUE(response.find_object_member("stop_event_id")->is_null());
  for (const auto *removed : {"active_stop_id", "active_stop_generation"})
    EXPECT_EQ(response.find_object_member(removed), nullptr) << removed;
  const auto *participant =
      &response.find_object_member("participants")->as_array().front();
  ASSERT_NE(participant->find_object_member("stop"), nullptr);
  EXPECT_TRUE(participant->find_object_member("stop")->is_null());

  state.stop_event_id = core::StopEventId(42U);
  core::ParticipantStopObservationDTO observation;
  observation.phase = core::StopPhase::kStopped;
  observation.attempt = 1U;
  observation.stop_event_id = core::StopEventId(42U);
  observation.mode = core::StopMode::kOrdinary;
  observation.attribution_class = core::StopAttributionClass::kOperator;
  state.participants.front().stop = observation;
  response = OperationStateValue(state);
  EXPECT_EQ(response.find_object_member("stop_event_id")->as_number(), 42.0);
  participant = &response.find_object_member("participants")->as_array().front();
  EXPECT_EQ(*participant->find_object_member("stop"),
            ParticipantStopObservationValue(observation));
}
} // namespace
} // namespace manyforge::roboplan_backend::host_detail
