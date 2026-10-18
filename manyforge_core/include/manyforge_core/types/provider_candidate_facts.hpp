#pragma once

#include <manyforge_core/types/component_lifecycle.hpp>
#include <variant>
#include <vector>

namespace manyforge::core {

/// Observation from the SAME original accepted CCLC child. This value is not
/// permission: installation retains the actual Apply guard through Kernel commit.
struct InstalledApplyChildCandidateSourceDTO final {
  RequestOwnerEpoch lifecycle_owner_epoch;
  ApplyComponentBringUpReferenceDTO parent;
  ComponentLifecycleAcceptedDTO acceptance;
  ContentDigest original_request_payload_digest;

  friend bool operator==(const InstalledApplyChildCandidateSourceDTO&,
                         const InstalledApplyChildCandidateSourceDTO&) = default;
};

/// The facade re-reads the full exact recorded plan for this transaction before
/// producing facts; this reference neither substitutes a last-plan query nor
/// asserts that its candidate is already publicly published.
struct InstalledNativeApplyCandidateSourceDTO final {
  RequestOwnerEpoch lifecycle_owner_epoch;
  NamedApplyPublicationTransactionDTO apply;

  friend bool operator==(const InstalledNativeApplyCandidateSourceDTO&,
                         const InstalledNativeApplyCandidateSourceDTO&) = default;
};

/// Actual paused installed candidate membership, qualified by the existing
/// coordinator/recorded-plan source and the host's original installation guard.
/// It deliberately has no public receipt, aggregate lifecycle state, driver
/// observation or command-readiness claim. Full mixed membership is retained;
/// only genuine provider domains enter the existing Kernel provider map.
struct InstalledProviderCandidateFactsDTO final {
  RetainedBundleId retained_bundle_id;
  GraphRevision graph_revision;
  ControllableComponentId controllable_component_id;
  ComponentLifecycleGeneration component_lifecycle_generation;
  std::vector<ResolvedLifecycleControlDomainScopeDTO> domains;
  std::vector<ResolvedLifecycleProviderOccurrenceDTO> provider_occurrences;
  std::variant<InstalledApplyChildCandidateSourceDTO,
               InstalledNativeApplyCandidateSourceDTO> source;

  friend bool operator==(const InstalledProviderCandidateFactsDTO&,
                         const InstalledProviderCandidateFactsDTO&) = default;
};

}  // namespace manyforge::core
