#pragma once

#include <manyforge_core/cclc_open_request.hpp>
#include <manyforge_core/ports/iactive_bundle_publication_store.hpp>
#include <manyforge_core/ports/icclc_journal.hpp>
#include <manyforge_core/ports/icclc_route_selection_current_snapshot_source.hpp>
#include <manyforge_deployment/verified_controller_lifecycle_expected_domain.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

#include <functional>
#include <vector>

namespace manyforge::deployment {

/// Exact current controller-lifecycle source selected for one retained domain.
struct ControllerLifecycleSourceOccurrenceDTO final {
  core::ControlDomainId control_domain_id;
  core::RuntimeUnitId source_runtime_unit_id;
  core::RuntimeEpoch source_runtime_epoch;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool
  operator==(const ControllerLifecycleSourceOccurrenceDTO &,
             const ControllerLifecycleSourceOccurrenceDTO &) = default;
};

/// Production read-only composition of one complete route-selection snapshot.
///
/// The source brackets one atomic journal read with exact active-publication
/// reads, independently reloads the retained controller product, and derives
/// the entire component/binding/resource fence from the retained domain. It
/// never accepts caller-supplied lifecycle, controller, or scope facts.
class VerifiedRouteSelectionCurrentSnapshotSource final
    : public core::ICCLCRouteSelectionCurrentSnapshotSource,
      public core::ICCLCRouteSelectionTemplateSource {
public:
  VerifiedRouteSelectionCurrentSnapshotSource(
      const RetainedBundle &retained_bundle,
      const core::IActiveBundlePublicationReader &active_publication_reader,
      core::ActiveBundlePublicationReceiptDTO expected_active_publication,
      core::ICCLCCurrentFactReader &current_fact_reader,
      const IVerifiedRetainedProductArtifactSource &artifact_source,
      std::vector<ControllerLifecycleSourceOccurrenceDTO> controller_sources,
      std::function<core::Result<
          std::optional<core::ActiveControlConfigurationDTO>>(
          const core::ControlDomainId &)>
          kernel_current_configuration_source = {});

  [[nodiscard]] core::Result<core::RouteSelectionCurrentSnapshot>
  ReadCurrentRouteSelectionSnapshot(
      const core::RetainedBundleId &expected_retained_bundle_id,
      const core::GraphRevision &expected_graph_revision,
      const core::DesiredControlConfigurationDTO &desired_configuration)
      const override;

  [[nodiscard]] core::Result<core::CCLCRouteSelectionTemplateSetDTO>
  ReadCurrentRouteSelectionTemplates(
      const core::ControllableComponentId &component_id) const override;

private:
  const RetainedBundle &retained_bundle_;
  const core::IActiveBundlePublicationReader &active_publication_reader_;
  core::ActiveBundlePublicationReceiptDTO expected_active_publication_;
  core::ICCLCCurrentFactReader &current_fact_reader_;
  const IVerifiedRetainedProductArtifactSource &artifact_source_;
  std::vector<ControllerLifecycleSourceOccurrenceDTO> controller_sources_;
  std::function<core::Result<
      std::optional<core::ActiveControlConfigurationDTO>>(
      const core::ControlDomainId &)>
      kernel_current_configuration_source_;
};

} // namespace manyforge::deployment
