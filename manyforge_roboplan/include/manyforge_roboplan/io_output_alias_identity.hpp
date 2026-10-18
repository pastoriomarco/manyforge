#pragma once

#include <manyforge_core/types/io_output.hpp>
#include <memory>
#include <vector>

namespace manyforge::roboplan_backend {

/// Private transport identity of an actual driver lifetime, never wire
/// authority. Weak ownership avoids the driver -> channel -> driver ownership
/// cycle.
struct IoOutputAliasIdentity final {
  core::IoSignalId signal_id;
  core::ResolvedLifecycleProviderOccurrenceDTO provider;
  std::weak_ptr<const void> actual_driver_owner;
};

/// Actual bounded transport metadata, not Apply or lifecycle authority.
struct IoOutputAliasState final {
  core::ActiveBundlePublicationGeneration active_publication_generation;
  std::vector<IoOutputAliasIdentity> aliases;
};

inline bool SameIoOutputDriverOwner(const std::weak_ptr<const void> &left,
                                    const std::weak_ptr<const void> &right) {
  const std::weak_ptr<const void> empty;
  const bool present = left.owner_before(empty) || empty.owner_before(left);
  return present && !left.owner_before(right) && !right.owner_before(left);
}

} // namespace manyforge::roboplan_backend
