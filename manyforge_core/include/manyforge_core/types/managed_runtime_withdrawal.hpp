#pragma once

#include <algorithm>
#include <cstdint>
#include <manyforge_core/types/projection_common.hpp>
#include <string>

namespace manyforge::core {

/// Positive Manager supervision evidence, not a native projection reply.
/// Captured for the exact predecessor before replacement and completed only
/// after its direct child has been reaped under the named Apply fence.
struct ManagedRuntimeWithdrawalDTO final {
  ApplyId apply_id;
  ApplyGeneration apply_generation;
  ContentDigest runtime_convergence_intent_digest;
  RuntimeUnitId runtime_unit_id;
  RuntimeEpoch runtime_epoch;
  ContentDigest product_identity;
  ContentDigest configuration_identity;
  std::string process_observer_instance_id;
  std::uint64_t process_observation_sequence;

  [[nodiscard]] bool valid() const noexcept {
    return !apply_id.empty() && apply_generation.nonzero() &&
           IsExactLowercaseSha256Digest(runtime_convergence_intent_digest.value()) &&
           !runtime_unit_id.empty() && !runtime_epoch.empty() &&
           IsExactLowercaseSha256Digest(product_identity.value()) &&
           IsExactLowercaseSha256Digest(configuration_identity.value()) &&
           !process_observer_instance_id.empty() && process_observer_instance_id.size() <= 256U &&
           std::all_of(process_observer_instance_id.begin(), process_observer_instance_id.end(),
                       [](unsigned char c) { return c >= 0x21U && c <= 0x7eU; }) &&
           process_observation_sequence > 0U;
  }

  friend bool operator==(const ManagedRuntimeWithdrawalDTO&,
                         const ManagedRuntimeWithdrawalDTO&) = default;
};

}  // namespace manyforge::core
