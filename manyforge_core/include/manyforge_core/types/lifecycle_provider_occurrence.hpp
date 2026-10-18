#pragma once

#include <manyforge_core/types/strong_id.hpp>
#include <manyforge_core/types/cell_graph.hpp>

namespace manyforge::core {

struct ResolvedLifecycleProviderOccurrenceDTO final {
  ControlDomainId control_domain_id;
  ControlDomainRevision control_domain_revision;
  ProviderInstanceId provider_instance_id;
  ProviderManifestId provider_manifest_id;
  ProviderManifestRevision provider_manifest_revision;
  RuntimeUnitId provider_runtime_unit_id;
  RuntimeEpoch provider_runtime_epoch;
  ContentDigest lifecycle_plan_catalog_digest;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const ResolvedLifecycleProviderOccurrenceDTO&,
                         const ResolvedLifecycleProviderOccurrenceDTO&) = default;
};

[[nodiscard]] inline bool ProviderDomainMatchesOccurrence(
    const ControlDomainId& domain, const ControlDomainRevision& revision,
    const ProviderControlDomainDTO& provider, const RuntimeEpoch& epoch,
    const ResolvedLifecycleProviderOccurrenceDTO& occurrence) noexcept {
  return occurrence.control_domain_id == domain &&
         occurrence.control_domain_revision == revision &&
         occurrence.provider_instance_id == provider.provider_instance_id &&
         occurrence.provider_manifest_id == provider.provider_manifest_id &&
         occurrence.provider_manifest_revision == provider.provider_manifest_revision &&
         occurrence.provider_runtime_unit_id == provider.provider_runtime_unit_id &&
         occurrence.provider_runtime_epoch == epoch &&
         occurrence.lifecycle_plan_catalog_digest == provider.lifecycle_plan_catalog_digest;
}


}  // namespace manyforge::core
