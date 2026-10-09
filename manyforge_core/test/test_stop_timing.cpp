#include <gtest/gtest.h>

#include <limits>
#include <manyforge_core/stop_timing.hpp>

namespace manyforge::core {
namespace {

StopEvidencePolicyDTO policy() {
  const StopBudgetPhaseId command("admit"), terminal("rest");
  auto result = StopEvidencePolicyDTO{
      StopEvidencePolicyId("policy"),
      StopEvidencePolicyRevision("revision"),
      StopEvidenceDisposition::kStoppable,
      ContainmentGranularity::kResource,
      StopDeadlineBudgetDTO{3'000'000'000,
                            200'000'000,
                            {{command, 300'000'000},
                             {terminal, 1'750'000'000},
                             {StopBudgetPhaseId("hard-admit"), 1'900'000'000},
                             {StopBudgetPhaseId("hard-rest"), 2'700'000'000}}},
      {{StopEscalationStageId("nominal"),
        StopEscalationStageKind::kNominal,
        ContainmentGranularity::kResource,
        {command, StopContainmentCommandKind::kControllerGoalCancel, std::nullopt, std::nullopt},
        StopEvidenceConjunction::kAllOf,
        {{StopEvidenceItemId("rest"),
          terminal,
          StopEvidenceRequirementKind::kControllerGoalTerminal,
          std::nullopt,
          std::nullopt,
          {StopEvidenceSourceKind::kControllerGoalStatus, 10'000'000, std::nullopt, {}},
          {StopEvidenceObservationKind::kPostCommandDwell, 100'000'000}}}}}};
  auto hard = result.escalation_stages.front();
  hard.stop_escalation_stage_id = StopEscalationStageId("hard");
  hard.stage_kind = StopEscalationStageKind::kSoftwareHard;
  hard.containment_command.command_phase_id = StopBudgetPhaseId("hard-admit");
  hard.evidence_requirements.front().phase_id = StopBudgetPhaseId("hard-rest");
  hard.evidence_requirements.front().evidence_item_id = StopEvidenceItemId("hard-rest");
  result.escalation_stages.push_back(std::move(hard));
  return result;
}

TEST(StopTimingTest, AbsencePreservesExactQualifiedBudget) {
  const auto original = policy();
  const auto result = resolve_stop_timing_budget(original, std::nullopt);
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_EQ(result.value(), *original.deadline_budget);
}

TEST(StopTimingTest, OperationalCutoffPreservesPositivePostFaultWorkWithoutMutatingPolicy) {
  const auto original = policy();
  const auto saved = StopTimingParametersDTO{250'000'000, 750'000'000, 3'000'000'000};
  const auto result = resolve_stop_timing_budget(original, saved);
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_EQ(result.value().phases[0].deadline_offset_ns, 250'000'000);
  EXPECT_EQ(result.value().phases[1].deadline_offset_ns, saved.operational_timeout_ns);
  EXPECT_EQ(result.value().phases[2].deadline_offset_ns - saved.operational_timeout_ns,
            150'000'000);
  EXPECT_EQ(
      result.value().phases[3].deadline_offset_ns - result.value().phases[2].deadline_offset_ns,
      800'000'000);
  EXPECT_EQ(result.value().outer_duration_ns, 4'250'000'000);
  EXPECT_EQ(result.value().kernel_completion_reserve_ns, 200'000'000);
  EXPECT_EQ(original, policy());
  EXPECT_TRUE(validate_controlled_stop_timing(saved, 1'000'000'000).ok());
}

TEST(StopTimingTest, NoApplyTimeRefusalComparesTimeoutWithProfileOrAllowances) {
  // Item 24: as `main`, the Stop has one deadline and its watchdog issues the
  // hard floor on overrun; a timeout shorter than the profile plus latency and
  // settling allowances is not refused in advance.
  const StopTimingParametersDTO saved{250'000'000, 750'000'000, 2'000'000'000};
  EXPECT_TRUE(validate_controlled_stop_timing(saved, 1'000'000'000).ok());
  EXPECT_TRUE(validate_controlled_stop_timing(saved, 1'000'000'001).ok());
  EXPECT_TRUE(validate_controlled_stop_timing(saved, 1'500'000'000).ok());
  EXPECT_TRUE(validate_controlled_stop_timing({250'000'000, 750'000'000, 1}, 1'000'000'000).ok());
  // Field shape and the supported range are still checked.
  EXPECT_FALSE(validate_controlled_stop_timing(saved, 0).ok());
  EXPECT_FALSE(validate_controlled_stop_timing({0, 0, 1}, 1'000'000'000).ok());
}

TEST(StopTimingTest, AuthoredDecimalDurationsUseOneNanosecondRoundingPolicy) {
  const auto duration = authored_stop_duration_ns(1.1);
  ASSERT_TRUE(duration.ok());
  EXPECT_EQ(duration.value(), 1'100'000'000);
  EXPECT_TRUE(
      validate_controlled_stop_timing({250'000'000, 750'000'000, 2'100'000'000}, duration.value())
          .ok());
  // No minimum-timeout refusal (item 24).
  EXPECT_TRUE(
      validate_controlled_stop_timing({250'000'000, 750'000'000, 2'099'999'999}, duration.value())
          .ok());
  EXPECT_EQ(authored_stop_duration_ns(1.100000001).value(), 1'100'000'001);
  EXPECT_EQ(authored_stop_duration_ns(0.0000000015).value(), 2);
  for (const auto invalid : {0.0, -1.0, 0.0000000001, 2.1, std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::quiet_NaN()})
    EXPECT_FALSE(authored_stop_duration_ns(invalid).ok());
}

TEST(StopTimingTest, SettlingIsNotComparedWithEvidenceDwell) {
  // Items 1/26 and 24: a Stop is finished when the controller reports its
  // braking finished; no rest dwell is reserved, so no settling rule exists.
  auto original = policy();
  auto extra = original.escalation_stages.front().evidence_requirements.front();
  extra.evidence_item_id = StopEvidenceItemId("another-rest");
  extra.observation_semantics.minimum_dwell_ns = 200'000'000;
  original.escalation_stages.front().evidence_requirements.push_back(extra);
  EXPECT_TRUE(resolve_stop_timing_budget(
                  original, StopTimingParametersDTO{100'000'000, 0, 2'000'000'000})
                  .ok());
  EXPECT_TRUE(resolve_stop_timing_budget(
                  original, StopTimingParametersDTO{100'000'000, 199'999'999, 2'000'000'000})
                  .ok());
  EXPECT_EQ(original.escalation_stages.front().evidence_requirements.back(), extra);
}

TEST(StopTimingTest, RejectsInvalidShapeOverflowAndConflictingPhaseAnchors) {
  for (const auto timing : {StopTimingParametersDTO{0, 0, 1},
                            {-1, 0, 1},
                            {1, -1, 2},
                            {1, 0, 0},
                            {1, 1, kMaxStopEvidencePolicyDurationNs}}) {
    EXPECT_FALSE(resolve_stop_timing_budget(policy(), timing).ok());
  }
  EXPECT_FALSE(
      validate_controlled_stop_timing({1, 1, 3}, kMaxStopEvidencePolicyDurationNs + 1).ok());
  auto malformed = policy();
  malformed.escalation_stages.front().evidence_requirements.front().phase_id =
      StopBudgetPhaseId("missing");
  EXPECT_FALSE(resolve_stop_timing_budget(malformed, StopTimingParametersDTO{1, 1, 3}).ok());
  malformed = policy();
  malformed.deadline_budget->phases[1].deadline_offset_ns = 1;
  EXPECT_FALSE(resolve_stop_timing_budget(malformed, std::nullopt).ok());
}

TEST(StopTimingTest, CommonEnvelopeDoesNotExtendIndependentTargetCutoffs) {
  auto first = resolve_stop_timing_budget(
      policy(), StopTimingParametersDTO{250'000'000, 750'000'000, 3'000'000'000});
  auto second = resolve_stop_timing_budget(
      policy(), StopTimingParametersDTO{100'000'000, 200'000'000, 2'000'000'000});
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(second.ok());
  std::vector<StopDeadlineBudgetDTO> budgets{first.value(), second.value()};
  ASSERT_TRUE(harmonize_stop_timing_budgets(budgets).ok());
  EXPECT_EQ(budgets[0].outer_duration_ns, budgets[1].outer_duration_ns);
  EXPECT_EQ(budgets[0].phases, first.value().phases);
  EXPECT_EQ(budgets[1].phases, second.value().phases);
  budgets[1].phases.back().deadline_offset_ns = kMaxStopEvidencePolicyDurationNs;
  const auto invalid = budgets;
  EXPECT_FALSE(harmonize_stop_timing_budgets(budgets).ok());
  EXPECT_EQ(budgets, invalid);
}

TEST(StopTimingTest, PreservedIntermediateEvidenceIsNotComparedWithItsDwell) {
  auto original = policy();
  original.deadline_budget->phases.front().deadline_offset_ns = 100'000'000;
  original.deadline_budget->phases.insert(original.deadline_budget->phases.begin() + 1,
                                          {StopBudgetPhaseId("intermediate"), 400'000'000});
  auto intermediate = original.escalation_stages.front().evidence_requirements.front();
  intermediate.evidence_item_id = StopEvidenceItemId("intermediate");
  intermediate.phase_id = StopBudgetPhaseId("intermediate");
  intermediate.observation_semantics.minimum_dwell_ns = 200'000'000;
  original.escalation_stages.front().evidence_requirements.push_back(intermediate);
  const auto result = resolve_stop_timing_budget(
      original, StopTimingParametersDTO{300'000'000, 200'000'000, 2'000'000'000});
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_EQ(result.value().phases[1], original.deadline_budget->phases[1]);
}

}  // namespace
}  // namespace manyforge::core
