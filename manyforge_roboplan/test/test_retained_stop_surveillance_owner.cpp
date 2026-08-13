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
      {RetainedStopSurveillanceSourceInput{binding, channel, {source}}});

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
     RejectsUnsealedOrAmbiguousSourceInventoryBeforePublication) {
  const auto binding = make_binding();
  auto channel = std::make_shared<StateChannel>(binding);
  const auto source = ObservationSource(binding);
  EXPECT_TRUE(
      RetainedStopSurveillanceOwner::ValidateSources(
          {RetainedStopSurveillanceSourceInput{binding, channel, {source}}})
          .ok());
  EXPECT_FALSE(RetainedStopSurveillanceOwner::ValidateSources(
                   {RetainedStopSurveillanceSourceInput{
                       binding, channel, {source, source}}})
                   .ok());

  auto wrong = source;
  wrong.resolved_source.runtime_target.provider_runtime_epoch =
      core::RuntimeEpoch("foreign.epoch");
  EXPECT_FALSE(
      RetainedStopSurveillanceOwner::ValidateSources(
          {RetainedStopSurveillanceSourceInput{binding, channel, {wrong}}})
          .ok());
}

} // namespace
} // namespace manyforge::roboplan_backend
