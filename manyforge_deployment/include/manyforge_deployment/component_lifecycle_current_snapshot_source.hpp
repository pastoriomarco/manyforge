#pragma once

#include <manyforge_core/ports/iactive_bundle_publication_store.hpp>
#include <manyforge_core/ports/icclc_current_component_snapshot_source.hpp>
#include <manyforge_core/ports/icclc_journal.hpp>
#include <manyforge_core/ports/icclc_lifecycle_plan_catalog_source.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

#include <vector>

namespace manyforge::deployment {

/// Production read-only composition of one exact current component snapshot.
///
/// Every input capability is read-only. The retained bundle and expected
/// active-publication receipt are immutable construction anchors; a later
/// active-head change fails the read and requires composition to construct a
/// new source. Provider occurrences are CCLC/composition-owned current runtime
/// facts and are independently joined to the retained graph and exact
/// build-owned lifecycle catalog on every read.
class VerifiedComponentLifecycleCurrentSnapshotSource final
    : public core::ICCLCCurrentComponentSnapshotSource {
public:
  VerifiedComponentLifecycleCurrentSnapshotSource(
      const RetainedBundle &retained_bundle,
      const core::IActiveBundlePublicationReader &active_publication_reader,
      core::ActiveBundlePublicationReceiptDTO expected_active_publication,
      core::ICCLCCurrentFactReader &current_fact_reader,
      const core::ICCLCLifecyclePlanCatalogSource &lifecycle_catalog_source,
      std::vector<core::ResolvedLifecycleProviderOccurrenceDTO>
          provider_occurrences);

  [[nodiscard]] core::Result<core::ComponentLifecycleCurrentSnapshotDTO>
  ReadCurrentComponentSnapshot(const core::ControllableComponentId
                                   &controllable_component_id) const override;

private:
  const RetainedBundle &retained_bundle_;
  const core::IActiveBundlePublicationReader &active_publication_reader_;
  core::ActiveBundlePublicationReceiptDTO expected_active_publication_;
  core::ICCLCCurrentFactReader &current_fact_reader_;
  const core::ICCLCLifecyclePlanCatalogSource &lifecycle_catalog_source_;
  std::vector<core::ResolvedLifecycleProviderOccurrenceDTO>
      provider_occurrences_;
};

} // namespace manyforge::deployment
