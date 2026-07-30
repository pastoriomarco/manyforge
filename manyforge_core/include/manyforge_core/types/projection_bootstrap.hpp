#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include <manyforge_core/types/active_bundle_publication.hpp>
#include <manyforge_core/types/projection_common.hpp>
#include <manyforge_core/types/retained_product.hpp>

namespace manyforge::core {

// The complete logical-scene value is owned by the scene-contract cutover.
// Bootstrap carries it as an immutable root so this Manager-facing substrate
// does not reuse the incompatible legacy LogicalSceneSnapshot shape.
struct LogicalSceneSnapshotDTO;

/// Non-null immutable ownership root for the future frozen logical-scene DTO.
///
/// Spec 220 permits shared immutable logical snapshots. Wrapping the shared
/// pointer prevents a required snapshot field from acquiring a second
/// "present-but-null" state while the scene DTO's implementation lands.
///
/// This bounded substrate cannot inspect the incomplete scene value and does
/// not prove its cell, revision, or content identity. Operational I4 bootstrap
/// remains unavailable until the scene-contract cutover supplies that
/// validation at the owning boundary.
class LogicalSceneSnapshotRoot final {
 public:
  explicit LogicalSceneSnapshotRoot(
      std::shared_ptr<const LogicalSceneSnapshotDTO> snapshot)
      : snapshot_(std::move(snapshot)) {
    if (!snapshot_) {
      throw std::invalid_argument(
          "logical scene snapshot root must be non-null");
    }
  }

  LogicalSceneSnapshotRoot(const LogicalSceneSnapshotRoot&) = default;
  LogicalSceneSnapshotRoot& operator=(
      const LogicalSceneSnapshotRoot&) = default;

  LogicalSceneSnapshotRoot(LogicalSceneSnapshotRoot&& other) noexcept
      : snapshot_(other.snapshot_) {}

  LogicalSceneSnapshotRoot& operator=(
      LogicalSceneSnapshotRoot&& other) noexcept {
    snapshot_ = other.snapshot_;
    return *this;
  }

  [[nodiscard]] const std::shared_ptr<const LogicalSceneSnapshotDTO>& shared()
      const noexcept {
    return snapshot_;
  }

 private:
  std::shared_ptr<const LogicalSceneSnapshotDTO> snapshot_;
};

struct ProjectionBootstrapIntentDTO final {
  BootstrapId bootstrap_id;
  BootstrapGeneration bootstrap_generation;
  CellId cell_id;
  RetainedBundleDTO canonical_empty_retained_bundle;
  std::vector<ProjectionNativeRuntimeBindingDTO> native_runtime_bindings;

  [[nodiscard]] bool valid() const noexcept {
    if (bootstrap_id.empty() || !bootstrap_generation.nonzero() ||
        cell_id.empty() ||
        canonical_empty_retained_bundle.retained_bundle_id.empty() ||
        canonical_empty_retained_bundle.cell_graph.cell_id != cell_id) {
      return false;
    }
    for (std::size_t index = 0U; index < native_runtime_bindings.size();
         ++index) {
      const auto& binding = native_runtime_bindings[index];
      if (!binding.valid()) {
        return false;
      }
      if (index != 0U &&
          !CanonicalProjectionNativeBindingLess(
              native_runtime_bindings[index - 1U], binding)) {
        return false;
      }
    }
    return true;
  }
};

/// Locally well-formed bootstrap acceptance.
///
/// `valid()` cannot inspect the incomplete logical-scene value. The scene
/// owner must validate its exact cell/revision/content relationship before
/// operational I4 may accept this record.
struct ProjectionBootstrapAcceptedDTO final {
  BootstrapId bootstrap_id;
  BootstrapGeneration bootstrap_generation;
  CellId cell_id;
  ProjectionRevisionDTO canonical_empty_target;
  LogicalSceneSnapshotRoot initialized_logical_scene;

  [[nodiscard]] bool valid() const noexcept {
    return !bootstrap_id.empty() && bootstrap_generation.nonzero() &&
           !cell_id.empty() && canonical_empty_target.valid();
  }
};

struct ProjectionBootstrapRebuildRequestDTO final {
  BootstrapId bootstrap_id;
  BootstrapGeneration bootstrap_generation;
  ProjectionRevisionDTO canonical_empty_target;

  [[nodiscard]] bool valid() const noexcept {
    return !bootstrap_id.empty() && bootstrap_generation.nonzero() &&
           canonical_empty_target.valid();
  }
};

enum class NativeProjectionBootstrapRebuildState {
  kRebuilt,
  kAlreadyExact,
  kFailedProjectionUncertain,
};

struct NativeProjectionBootstrapRebuildResultDTO final {
  BootstrapId bootstrap_id;
  BootstrapGeneration bootstrap_generation;
  ProjectionNativeRuntimeBindingDTO native_runtime_binding;
  ProjectionRevisionDTO canonical_empty_target;
  RetainedProductDTO planning_collision_model;
  LogicalSceneSnapshotRoot initialized_logical_scene;
  NativeProjectionBootstrapRebuildState state;
  std::optional<ProjectionRevisionDTO> acknowledged_target;
  std::optional<std::uint64_t> native_projection_generation;
  std::optional<ContentDigest> projection_content_digest;
  ProjectionHealthDTO resulting_health;
  std::optional<ErrorDTO> error;

  [[nodiscard]] bool presence_valid() const noexcept {
    if (bootstrap_id.empty() || !bootstrap_generation.nonzero() ||
        !native_runtime_binding.valid() || !canonical_empty_target.valid() ||
        planning_collision_model.product_id.empty() ||
        planning_collision_model.product_kind !=
            RetainedProductKind::kPlanningCollisionModel ||
        planning_collision_model.product_contract_id.empty() ||
        planning_collision_model.product_contract_revision.empty() ||
        !resulting_health.valid() ||
        resulting_health.native_runtime_binding != native_runtime_binding ||
        (native_projection_generation.has_value() &&
         *native_projection_generation == 0U) ||
        (projection_content_digest.has_value() &&
         projection_content_digest->empty())) {
      return false;
    }
    switch (state) {
      case NativeProjectionBootstrapRebuildState::kRebuilt:
      case NativeProjectionBootstrapRebuildState::kAlreadyExact:
        return acknowledged_target.has_value() &&
               *acknowledged_target == canonical_empty_target &&
               resulting_health.healthy_for_target(
                   canonical_empty_target) &&
               !error.has_value();
      case NativeProjectionBootstrapRebuildState::
          kFailedProjectionUncertain:
        return !acknowledged_target.has_value() && error.has_value();
    }
    return false;
  }
};

enum class NativeProjectionBootstrapStatusState {
  kAbsent,
  kRebuilding,
  kRebuilt,
  kFailedProjectionUncertain,
};

/// One positively observed native bootstrap state.
///
/// Unlike a rebuild result, status does not re-echo the retained product or
/// incomplete logical-scene value. It can therefore represent partial crash
/// recovery without manufacturing either required rebuild input.
struct NativeProjectionBootstrapStatusDTO final {
  BootstrapId bootstrap_id;
  BootstrapGeneration bootstrap_generation;
  ProjectionNativeRuntimeBindingDTO native_runtime_binding;
  ProjectionRevisionDTO canonical_empty_target;
  NativeProjectionBootstrapStatusState state;
  std::optional<ProjectionRevisionDTO> acknowledged_target;
  std::optional<std::uint64_t> native_projection_generation;
  std::optional<ContentDigest> projection_content_digest;
  ProjectionHealthDTO resulting_health;
  std::optional<ErrorDTO> error;

  [[nodiscard]] bool presence_valid() const noexcept {
    if (bootstrap_id.empty() || !bootstrap_generation.nonzero() ||
        !native_runtime_binding.valid() || !canonical_empty_target.valid() ||
        !resulting_health.valid() ||
        resulting_health.native_runtime_binding != native_runtime_binding ||
        (native_projection_generation.has_value() &&
         *native_projection_generation == 0U) ||
        (projection_content_digest.has_value() &&
         projection_content_digest->empty())) {
      return false;
    }
    switch (state) {
      case NativeProjectionBootstrapStatusState::kAbsent:
        return !acknowledged_target.has_value() &&
               resulting_health.state ==
                   ProjectionHealthState::kUninitialized &&
               !resulting_health.active_projection_revision.has_value() &&
               !resulting_health.staged_candidate.has_value() &&
               !resulting_health.error.has_value() &&
               !error.has_value();
      case NativeProjectionBootstrapStatusState::kRebuilding:
        return !acknowledged_target.has_value() &&
               resulting_health.state ==
                   ProjectionHealthState::kRebuilding &&
               !resulting_health.active_projection_revision.has_value() &&
               !resulting_health.staged_candidate.has_value() &&
               !resulting_health.error.has_value() &&
               !error.has_value();
      case NativeProjectionBootstrapStatusState::kRebuilt:
        return acknowledged_target.has_value() &&
               *acknowledged_target == canonical_empty_target &&
               resulting_health.healthy_for_target(
                   canonical_empty_target) &&
               !error.has_value();
      case NativeProjectionBootstrapStatusState::
          kFailedProjectionUncertain:
        return !acknowledged_target.has_value() && error.has_value();
    }
    return false;
  }
};

struct ProjectionBootstrapEvidenceDTO final {
  BootstrapId bootstrap_id;
  BootstrapGeneration bootstrap_generation;
  ProjectionRevisionDTO canonical_empty_target;
  std::vector<NativeProjectionBootstrapRebuildResultDTO> rebuild_results;
  ProjectionAggregateHealthDTO candidate_aggregate_health;

  /// Validates only locally available identities, row success, ordering, and
  /// target health. Required-set completeness remains a contextual verifier
  /// obligation against the persisted bootstrap intent.
  [[nodiscard]] bool locally_valid() const noexcept {
    if (bootstrap_id.empty() || !bootstrap_generation.nonzero() ||
        !canonical_empty_target.valid() ||
        !candidate_aggregate_health.matches_bootstrap(
            bootstrap_id, bootstrap_generation, canonical_empty_target) ||
        !candidate_aggregate_health.healthy_for_target(
            canonical_empty_target) ||
        rebuild_results.size() !=
            candidate_aggregate_health.native_projection_health.size()) {
      return false;
    }
    for (std::size_t index = 0U; index < rebuild_results.size(); ++index) {
      const auto& result = rebuild_results[index];
      if (!result.presence_valid() ||
          result.bootstrap_id != bootstrap_id ||
          result.bootstrap_generation != bootstrap_generation ||
          result.canonical_empty_target != canonical_empty_target ||
          result.native_runtime_binding !=
              candidate_aggregate_health.native_projection_health[index]
                  .native_runtime_binding ||
          (result.state != NativeProjectionBootstrapRebuildState::kRebuilt &&
           result.state !=
               NativeProjectionBootstrapRebuildState::kAlreadyExact)) {
        return false;
      }
      if (index != 0U &&
          !CanonicalProjectionNativeBindingLess(
              rebuild_results[index - 1U].native_runtime_binding,
              result.native_runtime_binding)) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] bool matches_bootstrap(
      const BootstrapId& expected_bootstrap_id,
      BootstrapGeneration expected_bootstrap_generation,
      const ProjectionRevisionDTO& expected_target) const noexcept {
    return locally_valid() && bootstrap_id == expected_bootstrap_id &&
           bootstrap_generation == expected_bootstrap_generation &&
           canonical_empty_target == expected_target;
  }

  [[nodiscard]] bool final_health_bindings_match(
      const ProjectionAggregateHealthDTO& final_health) const noexcept {
    return locally_valid() &&
           EqualCanonicalProjectionHealthBindings(
               candidate_aggregate_health, final_health);
  }
};

enum class ProjectionBootstrapRebuildResultState {
  kReadyToPublish,
  kFailedProjectionUncertain,
};

struct ProjectionBootstrapRebuildResultDTO final {
  BootstrapId bootstrap_id;
  BootstrapGeneration bootstrap_generation;
  ProjectionRevisionDTO canonical_empty_target;
  ProjectionBootstrapRebuildResultState state;
  std::optional<ProjectionBootstrapEvidenceDTO> bootstrap_evidence;
  std::optional<ErrorDTO> error;

  [[nodiscard]] bool presence_valid() const noexcept {
    if (bootstrap_id.empty() || !bootstrap_generation.nonzero() ||
        !canonical_empty_target.valid()) {
      return false;
    }
    switch (state) {
      case ProjectionBootstrapRebuildResultState::kReadyToPublish:
        return bootstrap_evidence.has_value() &&
               bootstrap_evidence->matches_bootstrap(
                   bootstrap_id, bootstrap_generation,
                   canonical_empty_target) &&
               !error.has_value();
      case ProjectionBootstrapRebuildResultState::
          kFailedProjectionUncertain:
        return !bootstrap_evidence.has_value() && error.has_value();
    }
    return false;
  }
};

struct ProjectionBootstrapFinalizeDTO final {
  BootstrapId bootstrap_id;
  BootstrapGeneration bootstrap_generation;
  ProjectionRevisionDTO canonical_empty_target;
  ActiveBundlePublicationReceiptDTO publication_receipt;

  [[nodiscard]] bool valid() const noexcept {
    return !bootstrap_id.empty() && bootstrap_generation.nonzero() &&
           canonical_empty_target.valid() &&
           publication_receipt.matches_bootstrap(
               bootstrap_id, bootstrap_generation,
               canonical_empty_target);
  }
};

enum class ProjectionBootstrapFinalizeResultState {
  kFinalized,
  kStale,
  kFailedProjectionUncertain,
};

struct ProjectionBootstrapFinalizeResultDTO final {
  BootstrapId bootstrap_id;
  BootstrapGeneration bootstrap_generation;
  ProjectionRevisionDTO canonical_empty_target;
  ActiveBundlePublicationReceiptDTO publication_receipt;
  ProjectionBootstrapFinalizeResultState state;
  std::optional<ProjectionBootstrapEvidenceDTO> bootstrap_evidence;
  std::optional<ProjectionAggregateHealthDTO> candidate_aggregate_health;
  std::optional<ErrorDTO> error;

  [[nodiscard]] bool presence_valid() const noexcept {
    if (bootstrap_id.empty() || !bootstrap_generation.nonzero() ||
        !canonical_empty_target.valid() ||
        !publication_receipt.matches_bootstrap(
            bootstrap_id, bootstrap_generation,
            canonical_empty_target)) {
      return false;
    }
    if (bootstrap_evidence.has_value() &&
        !bootstrap_evidence->matches_bootstrap(
            bootstrap_id, bootstrap_generation,
            canonical_empty_target)) {
      return false;
    }
    if (candidate_aggregate_health.has_value() &&
        !candidate_aggregate_health->matches_bootstrap(
            bootstrap_id, bootstrap_generation,
            canonical_empty_target)) {
      return false;
    }
    switch (state) {
      case ProjectionBootstrapFinalizeResultState::kFinalized:
        return bootstrap_evidence.has_value() &&
               candidate_aggregate_health.has_value() &&
               bootstrap_evidence->final_health_bindings_match(
                   *candidate_aggregate_health) &&
               candidate_aggregate_health->healthy_for_target(
                   canonical_empty_target) &&
               !error.has_value();
      case ProjectionBootstrapFinalizeResultState::kStale:
      case ProjectionBootstrapFinalizeResultState::
          kFailedProjectionUncertain:
        return !candidate_aggregate_health.has_value() &&
               error.has_value();
    }
    return false;
  }
};

struct ProjectionBootstrapStatusQueryDTO final {
  BootstrapId bootstrap_id;
  BootstrapGeneration bootstrap_generation;

  [[nodiscard]] bool valid() const noexcept {
    return !bootstrap_id.empty() && bootstrap_generation.nonzero();
  }
};

enum class ProjectionBootstrapStatusState {
  kAbsent,
  kAccepted,
  kRebuilding,
  kReadyToPublish,
  kPublished,
  kFinalized,
  kFailedProjectionUncertain,
};

struct ProjectionBootstrapStatusDTO final {
  BootstrapId bootstrap_id;
  BootstrapGeneration bootstrap_generation;
  ProjectionBootstrapStatusState state;
  std::optional<ProjectionRevisionDTO> canonical_empty_target;
  std::vector<NativeProjectionBootstrapStatusDTO>
      native_rebuild_observations;
  std::optional<ProjectionBootstrapEvidenceDTO> bootstrap_evidence;
  std::optional<ActiveBundlePublicationReceiptDTO> publication_receipt;
  std::optional<ProjectionAggregateHealthDTO> finalized_aggregate_health;
  std::optional<ErrorDTO> error;

  [[nodiscard]] bool presence_valid() const noexcept {
    if (bootstrap_id.empty() || !bootstrap_generation.nonzero()) {
      return false;
    }
    const bool has_target = canonical_empty_target.has_value();
    const bool has_observations = !native_rebuild_observations.empty();
    const bool has_evidence = bootstrap_evidence.has_value();
    const bool has_receipt = publication_receipt.has_value();
    const bool has_final_health = finalized_aggregate_health.has_value();
    if (has_target && !canonical_empty_target->valid()) {
      return false;
    }
    for (std::size_t index = 0U;
         index < native_rebuild_observations.size(); ++index) {
      const auto& observation = native_rebuild_observations[index];
      if (!has_target || !observation.presence_valid() ||
          observation.bootstrap_id != bootstrap_id ||
          observation.bootstrap_generation != bootstrap_generation ||
          observation.canonical_empty_target != *canonical_empty_target) {
        return false;
      }
      if (index != 0U &&
          !CanonicalProjectionNativeBindingLess(
              native_rebuild_observations[index - 1U]
                  .native_runtime_binding,
              observation.native_runtime_binding)) {
        return false;
      }
    }
    if (has_evidence &&
        (!has_target ||
         !bootstrap_evidence->matches_bootstrap(
             bootstrap_id, bootstrap_generation,
             *canonical_empty_target))) {
      return false;
    }
    if (has_receipt &&
        (!has_target ||
         !publication_receipt->matches_bootstrap(
             bootstrap_id, bootstrap_generation,
             *canonical_empty_target))) {
      return false;
    }
    if (has_final_health &&
        (!has_target ||
         !finalized_aggregate_health->matches_bootstrap(
             bootstrap_id, bootstrap_generation,
             *canonical_empty_target))) {
      return false;
    }
    switch (state) {
      case ProjectionBootstrapStatusState::kAbsent:
        return !has_target && !has_observations && !has_evidence &&
               !has_receipt && !has_final_health && !error.has_value();
      case ProjectionBootstrapStatusState::kAccepted:
        return has_target && !has_observations && !has_evidence &&
               !has_receipt && !has_final_health && !error.has_value();
      case ProjectionBootstrapStatusState::kRebuilding:
        return has_target && !has_evidence && !has_receipt &&
               !has_final_health && !error.has_value();
      case ProjectionBootstrapStatusState::kReadyToPublish:
        return has_target && !has_observations && has_evidence &&
               !has_receipt && !has_final_health && !error.has_value();
      case ProjectionBootstrapStatusState::kPublished:
        return has_target && !has_observations && has_evidence && has_receipt &&
               !has_final_health && !error.has_value();
      case ProjectionBootstrapStatusState::kFinalized:
        return has_target && !has_observations && has_evidence && has_receipt &&
               has_final_health &&
               bootstrap_evidence->final_health_bindings_match(
                   *finalized_aggregate_health) &&
               finalized_aggregate_health->healthy_for_target(
                   *canonical_empty_target) &&
               !error.has_value();
      case ProjectionBootstrapStatusState::kFailedProjectionUncertain:
        return error.has_value() && !has_evidence;
    }
    return false;
  }
};

}  // namespace manyforge::core
