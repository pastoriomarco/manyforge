#pragma once

#include <manyforge_core/types/configuration_fence.hpp>
#include <manyforge_core/types/projection_common.hpp>
#include <manyforge_core/types/strong_id.hpp>

namespace manyforge::component_manager {

/// Durable intent written after scene Finalize and before Kernel Release.
///
/// The complete compare-and-swap request is retained so a lost Release
/// response can be reconciled against the exact request rather than retried
/// from newly assembled state.
struct ManagerApplyFenceReleaseIntentDTO final {
  manyforge::core::ContentDigest scene_finalized_digest;
  manyforge::core::ContentDigest fence_acceptance_digest;
  manyforge::core::ConfigurationFenceReleaseDTO release_request;

  [[nodiscard]] bool valid() const noexcept;

  friend bool operator==(const ManagerApplyFenceReleaseIntentDTO&,
                         const ManagerApplyFenceReleaseIntentDTO&) = default;
};

/// Exact successful Kernel Release receipt bound to its durable intent.
struct ManagerApplyFenceReleasedDTO final {
  manyforge::core::ContentDigest fence_release_intent_digest;
  manyforge::core::ConfigurationFenceReleasedDTO released_fence;

  [[nodiscard]] bool valid() const noexcept;

  friend bool operator==(const ManagerApplyFenceReleasedDTO&,
                         const ManagerApplyFenceReleasedDTO&) = default;
};

enum class ManagerApplyTerminalOutcomeState {
  kSucceeded,
};

/// Immutable successful named-Apply terminal outcome.
///
/// Failed or uncertain Apply transactions deliberately have no terminal
/// success row. This record can exist only after exact scene Finalize,
/// publication, and Kernel-fence release records are durable.
struct ManagerApplyTerminalOutcomeDTO final {
  manyforge::core::ApplyId apply_id;
  manyforge::core::ApplyGeneration apply_generation;
  manyforge::core::ProjectionRevisionDTO candidate_projection_revision;
  manyforge::core::ContentDigest publication_receipt_digest;
  manyforge::core::ContentDigest scene_finalized_digest;
  manyforge::core::ContentDigest fence_released_digest;
  ManagerApplyTerminalOutcomeState state;

  [[nodiscard]] bool valid() const noexcept;

  friend bool operator==(const ManagerApplyTerminalOutcomeDTO&,
                         const ManagerApplyTerminalOutcomeDTO&) = default;
};

[[nodiscard]] const char* ToString(ManagerApplyTerminalOutcomeState state) noexcept;

}  // namespace manyforge::component_manager
