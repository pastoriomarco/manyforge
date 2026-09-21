#include "native_stop_qualification_fixture.hpp"

#include <algorithm>
#include <array>
#include <gtest/gtest.h>
#include <limits>
#include <manyforge_roboplan/retained_stop_surveillance_owner.hpp>

namespace manyforge::roboplan_backend {
namespace {

using qualification_fixture::make_binding;
using qualification_fixture::StateChannel;

[[nodiscard]] RetainedStopSurveillanceObservationSource
ObservationSource(const core::RuntimeParticipantBinding &binding) {
  const auto &path = *binding.value().control_path;
  const core::ResolvedStopRuntimeTargetDTO target{
      path.provider_instance_id,
      path.provider_manifest_id,
      path.provider_manifest_revision,
      path.provider_runtime_unit_id,
      core::ProductId("product.native"),
      core::ProductContractId("contract.native"),
      core::ProductContractRevision("contract-revision"),
      core::ContentDigest("sha256:" + std::string(64U, '1')),
      path.provider_runtime_epoch,
      binding.value().runtime_unit_epochs.front().runtime_epoch,
      core::EndpointRoleId("state.stop"),
      "state.stop",
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt};
  std::vector<core::ResourceId> resources;
  for (const auto &resource : binding.value().resources) {
    resources.push_back(resource.resource_id);
  }
  std::sort(resources.begin(), resources.end());
  return RetainedStopSurveillanceObservationSource{
      core::StopEvidenceRequirementDTO{
          core::StopEvidenceItemId("evidence.motion.rest"),
          core::StopBudgetPhaseId("phase.rest"),
          core::StopEvidenceRequirementKind::kMotionRest, std::nullopt,
          std::nullopt,
          core::StopEvidenceSourceRequirementDTO{
              core::StopEvidenceSourceKind::kBindingStateAuthority,
              100,
              core::EndpointRoleId("state.stop"),
              {path.state_authority_class}},
          core::StopEvidenceObservationDTO{
              core::StopEvidenceObservationKind::kPostCommandDwell, 20}},
      core::ResolvedStopEvidenceSourceDTO{
          core::StopEvidenceSourceKind::kBindingStateAuthority, target,
          path.state_authority_id, path.state_authority_class, std::nullopt},
      std::move(resources)};
}

[[nodiscard]] core::StopSurveillanceObservationQueryDTO
Query(const RetainedStopSurveillanceObservationSource &source,
      const std::int64_t evaluation_time_ns) {
  return core::StopSurveillanceObservationQueryDTO{
      core::StopCertificateRefDTO{core::StopCertificateId("certificate.1"),
                                  core::StopCertificateRevision(1U),
                                  core::StopId("stop.1"),
                                  core::StopGeneration(1U)},
      core::StopScopeEntryKeyDTO{
          core::StopId("stop.1"), core::StopGeneration(1U),
          core::StopParticipantScopeKeyDTO{
              core::OperationId("operation.1"),
              core::OperationParticipantId("participant.1")}},
      source.evidence_requirement, source.resolved_source, evaluation_time_ns};
}

TEST(RetainedStopSurveillanceOwnerTest,
     ReadsExactAddressedRestAndReportsMotionAndStaleness) {
  const auto binding = make_binding();
  auto channel = std::make_shared<StateChannel>(binding);
  const auto source = ObservationSource(binding);
  RetainedStopSurveillanceOwner owner(
      {RetainedStopSurveillanceSourceInput{binding, channel, {source}, {}}});

  ASSERT_TRUE(channel
                  ->publish_state({std::array<double, 2U>{0.0, 0.0}},
                                  {std::array<double, 2U>{0.0, 0.0}}, 100, 100)
                  .applied());
  const auto settled = owner.Observe(Query(source, 105));
  ASSERT_EQ(settled.availability,
            core::StopSurveillanceSourceAvailabilityDTO::kAvailable);
  ASSERT_TRUE(settled.evaluated_evidence.has_value());
  EXPECT_EQ(settled.evaluated_evidence->state,
            core::StopEvidenceItemStateDTO::kSatisfied);
  EXPECT_EQ(settled.evaluated_evidence->source_age_at_evaluation_ns, 5);

  ASSERT_TRUE(channel
                  ->publish_state({std::array<double, 2U>{0.0, 0.0}},
                                  {std::array<double, 2U>{0.0, 0.1}}, 110, 110)
                  .applied());
  const auto moving = owner.Observe(Query(source, 115));
  ASSERT_EQ(moving.availability,
            core::StopSurveillanceSourceAvailabilityDTO::kAvailable);
  ASSERT_TRUE(moving.evaluated_evidence.has_value());
  EXPECT_EQ(moving.evaluated_evidence->state,
            core::StopEvidenceItemStateDTO::kFailed);

  const auto stale = owner.Observe(Query(source, 211));
  EXPECT_EQ(stale.availability,
            core::StopSurveillanceSourceAvailabilityDTO::kUnavailable);
  EXPECT_FALSE(stale.evaluated_evidence.has_value());
  ASSERT_TRUE(stale.error.has_value());
  EXPECT_EQ(stale.error->code, "stop_surveillance_source_stale");

  ASSERT_TRUE(
      channel
          ->publish_state({std::array<double, 2U>{0.0, 0.0}},
                          {std::array<double, 2U>{
                              0.0, std::numeric_limits<double>::quiet_NaN()}},
                          220, 220)
          .applied());
  const auto nonfinite = owner.Observe(Query(source, 221));
  EXPECT_EQ(nonfinite.availability,
            core::StopSurveillanceSourceAvailabilityDTO::kInvalid);
  EXPECT_FALSE(nonfinite.evaluated_evidence.has_value());
}

TEST(RetainedStopSurveillanceOwnerTest,
     PositionsWithoutVelocityNeverProveRest) {
  const auto binding = make_binding();
  auto channel = std::make_shared<StateChannel>(binding);
  const auto source = ObservationSource(binding);
  RetainedStopSurveillanceOwner owner(
      {RetainedStopSurveillanceSourceInput{binding, channel, {source}, {}}});
  const std::array positions{0.0, 0.0};
  ASSERT_TRUE(channel->publish_state(positions, {}, 100, 100).applied());
  const auto observation = owner.Observe(Query(source, 101));
  EXPECT_EQ(observation.availability,
            core::StopSurveillanceSourceAvailabilityDTO::kUnavailable);
  EXPECT_FALSE(observation.evaluated_evidence.has_value());
  ASSERT_TRUE(observation.error.has_value());
  EXPECT_EQ(observation.error->code, "stop_surveillance_velocity_unavailable");
}

TEST(RetainedStopSurveillanceOwnerTest,
     RejectsUnsealedOrAmbiguousSourceInventoryBeforePublication) {
  const auto binding = make_binding();
  auto channel = std::make_shared<StateChannel>(binding);
  const auto source = ObservationSource(binding);
  EXPECT_TRUE(
      RetainedStopSurveillanceOwner::ValidateSources(
          {RetainedStopSurveillanceSourceInput{binding, channel, {source}, {}}})
          .ok());
  EXPECT_FALSE(RetainedStopSurveillanceOwner::ValidateSources(
                   {RetainedStopSurveillanceSourceInput{
                       binding, channel, {source, source}, {}}})
                   .ok());

  auto wrong = source;
  wrong.resolved_source.runtime_target.provider_runtime_epoch =
      core::RuntimeEpoch("foreign.epoch");
  EXPECT_FALSE(
      RetainedStopSurveillanceOwner::ValidateSources(
          {RetainedStopSurveillanceSourceInput{binding, channel, {wrong}, {}}})
          .ok());
}

TEST(RetainedStopSurveillanceOwnerTest,
     BuildsCommandFreeFreshOrphanFactsWithoutAStopPlan) {
  const auto binding = make_binding();
  auto channel = std::make_shared<StateChannel>(binding);
  bool active_goal = false;
  RetainedNoActiveGoalEvidenceSource goal_source =
      [&](const std::int64_t) -> core::Result<core::ContentDigest> {
    if (active_goal) {
      return core::Result<core::ContentDigest>::Err(
          core::ErrorDTO{"invalid_state_transition",
                         "test goal is active",
                         {{"reason", "test_goal_active"}}});
    }
    return core::Result<core::ContentDigest>::Ok(
        core::ContentDigest("sha256:" + std::string(64U, 'a')));
  };
  RetainedStopSurveillanceOwner owner(
      {RetainedStopSurveillanceSourceInput{binding, channel, {}, goal_source}});
  ASSERT_TRUE(channel
                  ->publish_state({std::array<double, 2U>{0.0, 0.0}},
                                  {std::array<double, 2U>{0.0, 0.0}}, 100, 100)
                  .applied());
  std::vector<core::ResourceId> resources;
  for (const auto &resource : binding.value().resources)
    resources.push_back(resource.resource_id);
  std::sort(resources.begin(), resources.end());

  const auto facts = owner.BuildOrphanResourceReconciliationFacts(
      core::RequestOwnerEpoch("owner.2"), core::CellId("cell.test"), 7U,
      resources, 105);
  ASSERT_TRUE(facts.ok()) << facts.error().message;
  EXPECT_EQ(facts.value().expected_operation_resource_generation, 7U);
  EXPECT_EQ(facts.value().resources.size(), resources.size());
  EXPECT_TRUE(core::orphan_resource_reconciliation_facts_field_error(
                  facts.value()) == std::nullopt);

  active_goal = true;
  const auto rejected = owner.BuildOrphanResourceReconciliationFacts(
      core::RequestOwnerEpoch("owner.2"), core::CellId("cell.test"), 7U,
      resources, 105);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().details.at("reason"), "test_goal_active");
}

TEST(RetainedStopSurveillanceOwnerTest,
     BuildsCommandFreeOrphanFactsFromRetainedIdlePredictedState) {
  const auto binding = make_binding();
  auto channel = std::make_shared<StateChannel>(binding);
  bool active_goal = false;
  RetainedNoActiveGoalEvidenceSource goal_source =
      [&](const std::int64_t) -> core::Result<core::ContentDigest> {
    if (active_goal) {
      return core::Result<core::ContentDigest>::Err(
          core::ErrorDTO{"invalid_state_transition",
                         "test goal is active",
                         {{"reason", "test_goal_active"}}});
    }
    return core::Result<core::ContentDigest>::Ok(
        core::ContentDigest("sha256:" + std::string(64U, 'd')));
  };
  RetainedStopSurveillanceOwner owner(
      {RetainedStopSurveillanceSourceInput{binding, channel, {}, goal_source}});
  ASSERT_TRUE(channel
                  ->publish_state({std::array<double, 2U>{0.0, 0.0}},
                                  {std::array<double, 2U>{0.0, 0.0}}, 100, 100)
                  .applied());
  std::vector<core::ResourceId> resources;
  for (const auto &resource : binding.value().resources) {
    resources.push_back(resource.resource_id);
  }
  std::sort(resources.begin(), resources.end());
  constexpr std::int64_t assessed_at_ns =
      100 + kRetainedNativeReconciliationFreshnessBudgetNs + 1;

  const auto facts = owner.BuildOrphanResourceReconciliationFacts(
      core::RequestOwnerEpoch("owner.2"), core::CellId("cell.test"), 7U,
      resources, assessed_at_ns);
  ASSERT_TRUE(facts.ok()) << facts.error().message;
  ASSERT_EQ(facts.value().resources.size(), resources.size());
  for (const auto &fact : facts.value().resources) {
    ASSERT_TRUE(fact.settlement.source_age_ns.has_value());
    ASSERT_TRUE(fact.settlement.freshness_budget_ns.has_value());
    EXPECT_EQ(*fact.settlement.source_age_ns, assessed_at_ns - 100);
    EXPECT_EQ(*fact.settlement.freshness_budget_ns,
              assessed_at_ns - 100 +
                  kRetainedNativeReconciliationFreshnessBudgetNs);
  }

  active_goal = true;
  const auto rejected = owner.BuildOrphanResourceReconciliationFacts(
      core::RequestOwnerEpoch("owner.2"), core::CellId("cell.test"), 7U,
      resources, assessed_at_ns);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().details.at("reason"), "test_goal_active");
}

TEST(RetainedStopSurveillanceOwnerTest,
     RejectsOrWaitsInsteadOfRelabelingAStaleEpochForOrphanFacts) {
  const auto binding = make_binding();
  auto channel = std::make_shared<StateChannel>(binding);
  const auto source = ObservationSource(binding);
  const auto goal_source = [](std::int64_t) {
    return core::Result<core::ContentDigest>::Ok(
        core::ContentDigest("sha256:" + std::string(64U, 'b')));
  };
  RetainedStopSurveillanceOwner owner(
      {RetainedStopSurveillanceSourceInput{binding, channel, {}, goal_source}});
  ASSERT_TRUE(channel
                  ->publish_state({std::array<double, 2U>{0.0, 0.0}},
                                  {std::array<double, 2U>{0.0, 0.0}}, 100, 100)
                  .applied());
  channel->set_source_epoch(core::RuntimeEpoch("stale.epoch"));
  std::vector<core::ResourceId> resources;
  for (const auto &resource : binding.value().resources) {
    resources.push_back(resource.resource_id);
  }
  std::sort(resources.begin(), resources.end());

  const auto rejected = owner.BuildOrphanResourceReconciliationFacts(
      core::RequestOwnerEpoch("owner.2"), core::CellId("cell.test"), 7U,
      resources, 105);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().code, "native_current_resource_identity_mismatch");

  channel->refresh_source_on_wait(true);
  const auto reconciled = owner.BuildOrphanResourceReconciliationFacts(
      core::RequestOwnerEpoch("owner.2"), core::CellId("cell.test"), 7U,
      resources, 105);
  ASSERT_TRUE(reconciled.ok()) << reconciled.error().message;
  ASSERT_EQ(reconciled.value().resources.size(), resources.size());
  for (const auto &fact : reconciled.value().resources) {
    EXPECT_EQ(fact.settlement.source_epoch,
              binding.value().control_path->provider_runtime_epoch);
  }
}

TEST(RetainedStopSurveillanceOwnerTest,
     RejectsFactsBeforeAChangedBindingIsPublishedToItsStateChannel) {
  const auto binding = make_binding();
  auto channel = std::make_shared<StateChannel>(binding);
  const auto goal_source = [](std::int64_t) {
    return core::Result<core::ContentDigest>::Ok(
        core::ContentDigest("sha256:" + std::string(64U, 'c')));
  };
  RetainedStopSurveillanceOwner owner(
      {RetainedStopSurveillanceSourceInput{binding, channel, {}, goal_source}});
  ASSERT_TRUE(channel
                  ->publish_state({std::array<double, 2U>{0.0, 0.0}},
                                  {std::array<double, 2U>{0.0, 0.0}}, 100, 100)
                  .applied());
  auto changed_binding_value = binding.value();
  ASSERT_TRUE(changed_binding_value.control_path.has_value());
  changed_binding_value.control_path->active_control_configuration_id =
      core::ActiveControlConfigurationId("active.reconstructed");
  changed_binding_value.control_path->provider_runtime_epoch =
      core::RuntimeEpoch("runtime.reconstructed");
  const auto changed_binding = core::verify_runtime_participant_binding(
      std::move(changed_binding_value));
  ASSERT_TRUE(changed_binding.ok()) << changed_binding.error().message;
  channel->rebind_context(changed_binding.value());

  std::vector<core::ResourceId> resources;
  for (const auto &resource : binding.value().resources) {
    resources.push_back(resource.resource_id);
  }
  std::sort(resources.begin(), resources.end());

  const auto rejected = owner.BuildOrphanResourceReconciliationFacts(
      core::RequestOwnerEpoch("owner.2"), core::CellId("cell.test"), 7U,
      resources, 105);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().code, "native_current_resource_identity_mismatch");
}

} // namespace
} // namespace manyforge::roboplan_backend
