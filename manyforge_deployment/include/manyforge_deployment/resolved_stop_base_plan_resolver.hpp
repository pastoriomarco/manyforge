#pragma once

#include <manyforge_core/ports/iresolved_stop_base_plan_repository.hpp>
#include <manyforge_core/route_selection_owner.hpp>
#include <manyforge_core/runtime_participant_stop_readiness.hpp>
#include <manyforge_core/types/active_control_configuration.hpp>
#include <manyforge_deployment/verified_controller_lifecycle_expected_domain.hpp>
#include <span>
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

/// Q48: the deployment-authored controlled (operator) Stop per-resource
/// maximum deceleration of one participant's retained route, read from the
/// retained control-domain product without an active configuration. The
/// activation-time controller configuration of the offline mock lane uses it
/// as each joint's `max_deceleration_on_cancel`. Fails closed when the route
/// has no stoppable plan with exactly one motion-controlled Stop stage that
/// bounds every participant resource.
[[nodiscard]] std::vector<std::pair<core::ResourceId, double>>
retained_controlled_stop_max_deceleration_rad_s2(
    const RetainedBundle &retained_bundle,
    const IVerifiedRetainedProductArtifactSource &artifact_source,
    const core::RuntimeParticipantBinding &participant_binding);

/// Persist and independently reload one resolved plan before publishing the
/// sealed readiness value consumed by Kernel Prepare. A repository failure or
/// any readback disagreement fails closed; the input plan is never returned as
/// a substitute for independent readback.
[[nodiscard]] core::Result<core::RuntimeParticipantStopReadiness>
store_and_verify_runtime_participant_stop_readiness(
    const RetainedBundle &retained_bundle, core::ResolvedStopBasePlanDTO plan,
    core::ComponentLifecycleGeneration component_lifecycle_generation,
    const core::ControllerLifecycleExpectedDomain &expected_domain,
    const core::RuntimeParticipantBinding &participant_binding,
    core::IResolvedStopBasePlanRepository &repository);

[[nodiscard]] inline core::Result<core::RuntimeParticipantStopReadiness>
store_and_verify_runtime_participant_stop_readiness(
    const RetainedBundle &retained_bundle, core::ResolvedStopBasePlanDTO plan,
    core::ComponentLifecycleGeneration component_lifecycle_generation,
    const VerifiedControllerLifecycleExpectedDomain &expected_domain,
    const core::RuntimeParticipantBinding &participant_binding,
    core::IResolvedStopBasePlanRepository &repository) {
  return store_and_verify_runtime_participant_stop_readiness(
      retained_bundle, std::move(plan), component_lifecycle_generation,
      expected_domain.sealed(), participant_binding, repository);
}

/// One exact selected-binding result produced before Kernel registration.
///
/// The binding is resealed against the newly reconciled active configuration;
/// the readiness value contains the independently reloaded immutable base
/// plan; and the row is the corresponding route-publication projection. No
/// public request field or desired-route value can substitute for an observed
/// current fact at this boundary.
struct VerifiedRouteReadinessParticipant final {
  core::RuntimeParticipantBinding participant_binding;
  core::RuntimeParticipantStopReadiness stop_readiness;
  core::RouteReadinessPublicationRowDTO readiness_row;
};

/// Rebind one already sealed physical participant to one exact reconciled
/// active configuration. Program/role aliases and physical provider facts are
/// preserved; only the selected route, activation set, active-configuration
/// identity/generation, route-specific scaling qualification, and the
/// authoritative controller-domain runtime epoch are replaced.
[[nodiscard]] core::Result<core::RuntimeParticipantBinding>
rebind_runtime_participant_to_active_configuration(
    const RetainedBundle &retained_bundle,
    const core::ControllerLifecycleExpectedDomain &expected_domain,
    const core::ActiveControlConfigurationDTO &active_configuration,
    const core::RuntimeParticipantBinding &participant_binding);

[[nodiscard]] inline core::Result<core::RuntimeParticipantBinding>
rebind_runtime_participant_to_active_configuration(
    const RetainedBundle &retained_bundle,
    const VerifiedControllerLifecycleExpectedDomain &expected_domain,
    const core::ActiveControlConfigurationDTO &active_configuration,
    const core::RuntimeParticipantBinding &participant_binding) {
  return rebind_runtime_participant_to_active_configuration(
      retained_bundle, expected_domain.sealed(), active_configuration,
      participant_binding);
}

/// Resolve, persist, independently reload, and seal the complete readiness
/// set for one reconciled domain configuration.
///
/// `participant_bindings` contains exactly one physical representative for
/// every selected control binding. All plans are resolved and their common
/// outer-duration/Kernel-reserve envelope is checked before the first
/// repository mutation. Per-participant phase allocations remain distinct.
[[nodiscard]] core::Result<std::vector<VerifiedRouteReadinessParticipant>>
resolve_store_and_verify_route_readiness_batch(
    const RetainedBundle &retained_bundle,
    const IVerifiedRetainedProductArtifactSource &artifact_source,
    const core::ControllerLifecycleExpectedDomain &expected_domain,
    const core::ActiveControlConfigurationDTO &active_configuration,
    std::span<const core::RouteSelectionComponentExpectationDTO>
        component_expectations,
    std::span<const core::RuntimeParticipantBinding> participant_bindings,
    core::IResolvedStopBasePlanRepository &repository);

[[nodiscard]] inline core::Result<
    std::vector<VerifiedRouteReadinessParticipant>>
resolve_store_and_verify_route_readiness_batch(
    const RetainedBundle &retained_bundle,
    const IVerifiedRetainedProductArtifactSource &artifact_source,
    const VerifiedControllerLifecycleExpectedDomain &expected_domain,
    const core::ActiveControlConfigurationDTO &active_configuration,
    std::span<const core::RouteSelectionComponentExpectationDTO>
        component_expectations,
    std::span<const core::RuntimeParticipantBinding> participant_bindings,
    core::IResolvedStopBasePlanRepository &repository) {
  return resolve_store_and_verify_route_readiness_batch(
      retained_bundle, artifact_source, expected_domain.sealed(),
      active_configuration, component_expectations, participant_bindings,
      repository);
}

} // namespace manyforge::deployment
