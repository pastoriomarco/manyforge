#pragma once

#include <manyforge_core/runtime_participant_stop_readiness.hpp>
#include <manyforge_core/types/active_control_configuration.hpp>
#include <manyforge_deployment/verified_controller_lifecycle_expected_domain.hpp>
#include <utility>
#include <vector>

namespace manyforge::deployment {

/// Resolve one exact active-configuration-scoped Stop base plan from already
/// verified retained and current owner facts.
///
/// This NRT composition boundary performs no mutation and accepts no public
/// request input. The returned value is independently validated and carries
/// every runtime target, endpoint, qualification, evidence source, scope, and
/// deadline needed by Kernel Prepare.
[[nodiscard]] core::ResolvedStopBasePlanDTO resolve_verified_stop_base_plan(
    const RetainedBundle &retained_bundle,
    const IVerifiedRetainedProductArtifactSource &artifact_source,
    const core::ControllerLifecycleExpectedDomain &expected_domain,
    const core::ActiveControlConfigurationDTO &active_configuration,
    const core::RuntimeParticipantBinding &participant_binding);

[[nodiscard]] inline core::ResolvedStopBasePlanDTO
resolve_verified_stop_base_plan(
    const RetainedBundle &retained_bundle,
    const IVerifiedRetainedProductArtifactSource &artifact_source,
    const VerifiedControllerLifecycleExpectedDomain &expected_domain,
    const core::ActiveControlConfigurationDTO &active_configuration,
    const core::RuntimeParticipantBinding &participant_binding) {
  return resolve_verified_stop_base_plan(
      retained_bundle, artifact_source, expected_domain.sealed(),
      active_configuration, participant_binding);
}

/// Verify one resolved plan and seal the readiness value consumed by Kernel
/// Prepare. The in-memory plan is used as resolved; it is not persisted.
[[nodiscard]] core::Result<core::RuntimeParticipantStopReadiness>
seal_runtime_participant_stop_readiness(
    const RetainedBundle &retained_bundle, core::ResolvedStopBasePlanDTO plan,
    core::ComponentLifecycleGeneration component_lifecycle_generation,
    const core::ControllerLifecycleExpectedDomain &expected_domain,
    const core::RuntimeParticipantBinding &participant_binding);

[[nodiscard]] inline core::Result<core::RuntimeParticipantStopReadiness>
seal_runtime_participant_stop_readiness(
    const RetainedBundle &retained_bundle, core::ResolvedStopBasePlanDTO plan,
    core::ComponentLifecycleGeneration component_lifecycle_generation,
    const VerifiedControllerLifecycleExpectedDomain &expected_domain,
    const core::RuntimeParticipantBinding &participant_binding) {
  return seal_runtime_participant_stop_readiness(
      retained_bundle, std::move(plan), component_lifecycle_generation,
      expected_domain.sealed(), participant_binding);
}

} // namespace manyforge::deployment
