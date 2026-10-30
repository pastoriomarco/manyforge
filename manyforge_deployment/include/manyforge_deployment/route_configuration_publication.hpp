#pragma once

#include <cstdint>
#include <functional>
#include <manyforge_core/ports/icclc_route_configuration_publication_port.hpp>
#include <manyforge_deployment/resolved_stop_base_plan_resolver.hpp>
#include <span>
#include <vector>

namespace manyforge::deployment {

/// Narrow L0.4c seam owned by the stable execution composition.
///
/// It owns engine/adapter lifetime, alias expansion, the atomic Kernel
/// registration change, and its readback. It owns no retained-product decode,
/// route choice, base-plan resolution, or lifecycle generation.
class IRouteConfigurationKernelPublicationOwner {
public:
  virtual ~IRouteConfigurationKernelPublicationOwner() = default;

  [[nodiscard]] virtual core::Result<
      std::vector<core::RuntimeParticipantBinding>>
  ReadCurrentPhysicalParticipants(
      const core::ActiveControlConfigurationDTO &configuration) const = 0;

  [[nodiscard]] virtual core::Result<core::RuntimeRouteConfigurationReadbackDTO>
  PublishAndReadBackRouteConfiguration(
      const core::ActiveControlConfigurationDTO &configuration,
      std::vector<VerifiedRouteReadinessParticipant> participants) = 0;
};

/// Production NRT implementation of CCLC's route-configuration publication
/// port for one exact retained-bundle owner epoch.
class VerifiedRouteConfigurationPublicationPort final
    : public core::ICCLCRouteConfigurationPublicationPort {
public:
  VerifiedRouteConfigurationPublicationPort(
      const RetainedBundle &retained_bundle,
      const IVerifiedRetainedProductArtifactSource &artifact_source,
      IRouteConfigurationKernelPublicationOwner &kernel_owner,
      std::vector<core::KinematicResourceSourceProjectionDTO>
          source_projections,
      std::function<std::int64_t()> now_ns);

  [[nodiscard]] core::Result<core::ActiveControlConfigurationDTO>
  ResolveResultingActiveControlConfiguration(
      const core::RouteSelectionAcceptedDTO &acceptance,
      const core::ControllerLifecycleExpectedDomain &expected_controller_domain,
      const core::ControllerLifecycleObservationDTO &observation,
      const core::ControllerLifecycleReconciliationDTO &reconciliation,
      core::RouteSelectionEffectDTO transaction_effect) const override;

  [[nodiscard]] core::Result<core::RouteReadinessPublicationReceiptDTO>
  PublishAndReadBack(
      const core::RouteSelectionAcceptedDTO &acceptance,
      const core::ControllerLifecycleExpectedDomain &expected_controller_domain,
      const core::ActiveControlConfigurationDTO &durable_configuration)
      override;

private:
  const RetainedBundle &retained_bundle_;
  const IVerifiedRetainedProductArtifactSource &artifact_source_;
  IRouteConfigurationKernelPublicationOwner &kernel_owner_;
  std::vector<core::KinematicResourceSourceProjectionDTO> source_projections_;
  std::function<std::int64_t()> now_ns_;
};

} // namespace manyforge::deployment
