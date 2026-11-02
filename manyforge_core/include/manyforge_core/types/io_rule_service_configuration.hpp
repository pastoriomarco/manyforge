#pragma once

#include <manyforge_core/types/io_signal.hpp>
#include <cstddef>

namespace manyforge::core {

/// Immutable identity and the existing shared watch/rule registration bound.
/// Reading this requires no sample and grants no source qualification.
struct IoRuleServiceConfigurationDTO final {
  RequestOwnerEpoch owner_epoch;
  std::uint64_t catalog_generation;
  std::size_t maximum_registrations;
};

/// Last actual evaluation of this registration, separate from retained first
/// trip/loss. Null is unknown/unqualified, never false. Kernel evaluates on the
/// existing ingress/expiry schedule before reading this snapshot.
struct IoRuleConditionSnapshotDTO final {
  std::optional<bool> value;
  TimePoint evaluated_at_ns;
  friend bool operator==(const IoRuleConditionSnapshotDTO&,
                         const IoRuleConditionSnapshotDTO&) = default;
};

}  // namespace manyforge::core
