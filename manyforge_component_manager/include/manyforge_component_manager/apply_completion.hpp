#pragma once

#include <manyforge_core/types/configuration_fence.hpp>
#include <manyforge_core/types/projection_common.hpp>
#include <manyforge_core/types/strong_id.hpp>

#include <cstddef>
#include <map>
#include <optional>
#include <string>

namespace manyforge::component_manager {

/// Durable intent written after Scene maintenance completion and before Kernel
/// Release.
///
/// The complete compare-and-swap request is retained so a lost Release
/// response can be reconciled against the exact request rather than retried
/// from newly assembled state.
struct ManagerApplyFenceReleaseIntentDTO final {
  manyforge::core::ContentDigest scene_completion_digest;
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
  kFailed,
  kUnknownEffect,
};

/// Deepest durable milestone reached by one Apply generation.
///
/// This is a pure projection over the presence of the journal's immutable
/// parent-linked records. The journal stores no mutable phase label, and this
/// projection never infers a label from process responsiveness, heartbeat
/// traffic or wall time: a phase is reached only when its exact durable record
/// exists. Intermediate records (`fence_attempt`, `runtime_convergence_intent`,
/// `publication_receipt`, `fence_release_intent`) are implied by the milestone
/// that follows them in the parent chain.
enum class ManagerApplyPhase {
  kAccepted,
  kFenceAcquired,
  kFenceAccepted,
  kRuntimeConverged,
  kComposed,
  kPublished,
  kSettled,
};

[[nodiscard]] const char *ToString(ManagerApplyPhase phase) noexcept;

/// Whether a retained failure's effects are exactly established.
enum class ManagerApplyEffectDisposition {
  /// Every effect of the operation is exactly accounted for.
  kKnown,
  /// One or more effects could not be established and remain unknown.
  kUnknown,
};

[[nodiscard]] const char *ToString(ManagerApplyEffectDisposition disposition) noexcept;

/// Whether a retained failure happened before any configuration change.
///
/// Stated by the failure's source, never inferred from codes or phases.
enum class ManagerApplyEffectBoundary {
  /// The Apply made no configuration, runtime, component or Scene change. An
  /// admission claim, a Kernel fence and fail-closed readiness may still
  /// exist; whoever releases them must check those facts directly. The effect
  /// disposition is then `kKnown`.
  kBeforeEffect,
  /// The source cannot prove the Apply made no such change.
  kUnknown,
};

[[nodiscard]] const char *ToString(ManagerApplyEffectBoundary boundary) noexcept;

inline constexpr std::size_t kMaximumManagerApplyFailureCodeBytes = 128U;
inline constexpr std::size_t kMaximumManagerApplyFailureMessageBytes = 1024U;
inline constexpr std::size_t kMaximumManagerApplyFailureProducerBytes = 64U;
inline constexpr std::size_t kMaximumManagerApplyFailureCauseDetails = 16U;
inline constexpr std::size_t kMaximumManagerApplyFailureCauseDetailKeyBytes = 64U;
inline constexpr std::size_t kMaximumManagerApplyFailureCauseDetailValueBytes = 256U;

/// The first meaningful failure retained by one terminal Apply operation.
///
/// `failure_code` names the Apply phase that failed. The inner cause keeps its
/// own typed code and details: `cause_code` equals `failure_code` when the
/// inner failure carried no code. `failure_message` is the producer's message
/// and `producer` the owner that produced the cause (`component_manager`,
/// `kernel`, ...).
struct ManagerApplyFailureEvidenceDTO final {
  /// Exact intent of the operation that retained this failure.
  manyforge::core::ContentDigest intent_digest;
  /// Deepest durably reached phase at the moment the failure was retained.
  ManagerApplyPhase phase;
  std::string failure_code;
  std::string failure_message;
  ManagerApplyEffectDisposition effect_disposition;
  std::string cause_code;
  std::map<std::string, std::string> cause_details;
  std::string producer;
  ManagerApplyEffectBoundary effect_boundary;

  [[nodiscard]] bool valid() const noexcept;

  friend bool operator==(const ManagerApplyFailureEvidenceDTO &,
                         const ManagerApplyFailureEvidenceDTO &) = default;
};

/// The one immutable terminal outcome of one named-Apply generation.
///
/// Exactly one disposition exists per operation. A success carries the exact
/// Scene completion, publication and Kernel-fence-release evidence and no failure. A
/// `failed` or `unknown_effect` outcome carries the first retained failure with
/// its effect certainty and no publication evidence, because the operation
/// never completed Scene maintenance, publication and fence release. A terminal
/// failure is never repaired, retried or rolled back, and its effects are never
/// assumed absent.
struct ManagerApplyTerminalOutcomeDTO final {
  manyforge::core::ApplyId apply_id;
  manyforge::core::ApplyGeneration apply_generation;
  std::optional<manyforge::core::ProjectionRevisionDTO>
      candidate_projection_revision;
  std::optional<manyforge::core::ContentDigest> publication_receipt_digest;
  std::optional<manyforge::core::ContentDigest> scene_completion_digest;
  std::optional<manyforge::core::ContentDigest> fence_released_digest;
  ManagerApplyTerminalOutcomeState state;
  std::optional<ManagerApplyFailureEvidenceDTO> failure;

  [[nodiscard]] bool succeeded() const noexcept {
    return state == ManagerApplyTerminalOutcomeState::kSucceeded;
  }

  [[nodiscard]] bool valid() const noexcept;

  friend bool operator==(const ManagerApplyTerminalOutcomeDTO &,
                         const ManagerApplyTerminalOutcomeDTO &) = default;
};

[[nodiscard]] const char* ToString(ManagerApplyTerminalOutcomeState state) noexcept;

}  // namespace manyforge::component_manager
