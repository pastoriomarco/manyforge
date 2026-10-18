#include "execution_host_responses.hpp"
#include "io_rule_outcome_fixture.hpp"
#include <gtest/gtest.h>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/io_rule_outcome_codec.hpp>
#include <utility>

namespace manyforge::roboplan_backend::host_detail {
namespace {
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
      .surveillance_projection = {core::StopDurableProjectionStateDTO::kNotApplicable,
                                  std::nullopt, std::nullopt},
      .stop_certificate = std::nullopt, .fault_latch = std::nullopt,
      .surveillance = std::nullopt};
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
          {"api_version", Value("manyforge.execution_command.v1")},
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
