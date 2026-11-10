#include "execution_host_responses.hpp"
#include "io_rule_outcome_fixture.hpp"
#include <gtest/gtest.h>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/io_rule_outcome_codec.hpp>
#include <manyforge_core/types/typed_error.hpp>
#include <utility>

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

TEST(IoRuleResultResponseTest,
     PublicProjectionUsesSameClosedEvidenceAsDurableCodec) {
  const auto state = core::rule_codec_test::State();
  const auto response = OperationStateValue(state);
  const auto &participant =
      response.find_object_member("participants")->as_array().front();
  EXPECT_EQ(participant.find_object_member("command_id")->as_string(),
            state.participants.front().command_id.value());
  EXPECT_EQ(participant.find_object_member("command_generation")->as_number(),
            static_cast<double>(state.participants.front().command_generation.value()));
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
            "succeeded_escalated");
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
      core::ControllerGoalId("foreign.goal");
  EXPECT_THROW((void)OperationStateValue(state), std::exception);
}
TEST(OperationStateResponseTest, AbsentTakeoverKeepsLegacyShape) {
  const auto response = OperationStateValue(core::rule_codec_test::State());
  EXPECT_EQ(response.find_object_member("motion_takeover"), nullptr);
}

TEST(OperationStateResponseTest, TakeoverProjectsExactDecisionAndCertificate) {
  auto state = core::rule_codec_test::State();
  state.motion_takeover = core::MotionTakeoverDecisionDTO{
      .incumbent_operation_id = core::OperationId("operation.incumbent"),
      .path = core::MotionTakeoverPathDTO::kMovingReplacement,
      .cause = std::nullopt,
      .decided_at_ns = INT64_MAX,
      .elapsed_since_acceptance_ns = 9007199254740993LL,
      .preparation_budget_ns = 1500000000,
      .stop_certificate = std::nullopt,
      .planning_budget_ns = 1200000000};
  auto response = OperationStateValue(state);
  const auto *decision = response.find_object_member("motion_takeover");
  ASSERT_NE(decision, nullptr);
  EXPECT_EQ(decision->as_object().size(), 8U);
  EXPECT_EQ(decision->find_object_member("incumbent_operation_id")->as_string(),
            "operation.incumbent");
  EXPECT_EQ(decision->find_object_member("path")->as_string(),
            "moving_replacement");
  EXPECT_EQ(content_identity::canonical_json(
                *decision->find_object_member("cause")), "null");
  EXPECT_EQ(content_identity::canonical_json(
                *decision->find_object_member("stop_certificate")), "null");
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
  EXPECT_TRUE(decision->find_object_member("stop_certificate")->is_null());
  EXPECT_EQ(content_identity::canonical_json(
                *decision->find_object_member("cause")),
            content_identity::canonical_json(ErrorValue(cause)));
  state.motion_takeover->stop_certificate = core::StopCertificateRefDTO{
      core::StopCertificateId("certificate.actual"),
      core::StopCertificateRevision(3U), core::StopId("stop.actual"),
      core::StopGeneration(2U)};
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
    EXPECT_EQ(content_identity::canonical_json(
                  *decision->find_object_member("stop_certificate")),
              "{\"stop_certificate_id\":\"certificate.actual\","
              "\"stop_certificate_revision\":3,\"stop_generation\":2,"
              "\"stop_id\":\"stop.actual\"}");
  }
}
// Typed wire fixture only: projection status does not manufacture a
// certificate, attempted-stage evidence, a commit sequence or a new wait budget.
TEST(IoRuleResultResponseTest,
     StopSummarySeparatesTerminalMotionFromPendingDurableProjection) {
  const auto digest = "sha256:" + std::string(64U, 'a');
  core::StopStatusDTO status{
      .accepted = {core::RequestId("request.stop"),
                   core::RequestReplayStatusDTO::kAcceptedNew,
                   core::StopId("stop.actual"), core::StopGeneration(17U),
                   core::ContentDigest(digest),
                   core::StopResolutionDispositionDTO::kExact,
                   core::StopScopeResolutionReasonDTO::kExplicitFullStop,
                   {}, core::ContentDigest(digest), {}, {}, 10},
      .state = core::StopStatusStateDTO::kSucceeded,
      .scope_entries = {}, .current_stages = {}, .attempted_stage_results = {},
      .evidence_items = {}, .unsettled = {}, .failed = {},
      .acceptance_projection = {core::StopDurableProjectionStateDTO::kRecorded,
                                std::nullopt, std::nullopt},
      .terminal_projection = {core::StopDurableProjectionStateDTO::kPending,
                              std::nullopt, std::nullopt},
      .stop_certificate = std::nullopt};
  for (const auto &[state, spelling] :
       {std::pair{core::StopDurableProjectionStateDTO::kPending, "pending"},
        std::pair{core::StopDurableProjectionStateDTO::kRecorded, "recorded"},
        std::pair{core::StopDurableProjectionStateDTO::kFailed, "failed"}}) {
    status.terminal_projection.state = state;
    const auto response = StopStatusSummaryValue(status);
    EXPECT_EQ(response.as_object().size(), 6U);
    EXPECT_EQ(response.find_object_member("terminal_projection")->as_string(), spelling);
    EXPECT_EQ(response.find_object_member("state")->as_string(), "succeeded");
    EXPECT_EQ(response.find_object_member("stop_id")->as_string(), "stop.actual");
    EXPECT_EQ(response.find_object_member("stop_generation")->as_number(), 17.0);
    EXPECT_EQ(response.find_object_member("request_id")->as_string(), "request.stop");
    EXPECT_EQ(response.find_object_member("schema_version")->as_string(),
              "manyforge.execution_stop_status.v1");
    EXPECT_EQ(response.find_object_member("attempted_stages"), nullptr);
  }
  status.state = core::StopStatusStateDTO::kStopping;
  status.terminal_projection.state = core::StopDurableProjectionStateDTO::kPending;
  const auto active = StopStatusSummaryValue(status);
  EXPECT_EQ(active.find_object_member("state")->as_string(), "stopping");
  EXPECT_EQ(active.find_object_member("terminal_projection")->as_string(), "pending");
  for (const auto unsupported :
       {core::StopDurableProjectionStateDTO::kNotApplicable,
        static_cast<core::StopDurableProjectionStateDTO>(255)}) {
    status.terminal_projection.state = unsupported;
    EXPECT_THROW((void)StopStatusSummaryValue(status), std::exception);
  }
  using Value = content_identity::CanonicalValue;
  const auto command = DecodeCommand(content_identity::canonical_json(
      Value(Value::Object{
          {"api_version", Value("manyforge.execution_command.v3")},
          {"command", Value("read_stop")},
          {"stop_id", Value("stop.actual")},
          {"attribution", Value(Value::Object{
              {"transport", Value("internal")},
              {"assurance", Value("unverified")},
              {"principal_id", Value(nullptr)}})}})));
  EXPECT_EQ(command.kind, CommandKind::kReadStop);
  EXPECT_EQ(command.stop_id, core::StopId("stop.actual"));
}
} // namespace
} // namespace manyforge::roboplan_backend::host_detail
