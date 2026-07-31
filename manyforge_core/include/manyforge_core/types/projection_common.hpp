#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/strong_id.hpp>

namespace manyforge::core {

inline constexpr std::string_view kSha256DigestPrefix = "sha256:";

/// Exact portable digest spelling used by scene and projection identities.
///
/// This predicate deliberately validates representation only. It neither
/// computes nor vouches for the digest's semantic preimage.
[[nodiscard]] constexpr bool IsExactLowercaseSha256Digest(
    const std::string_view value) noexcept {
  if (value.size() != kSha256DigestPrefix.size() + 64U ||
      value.substr(0U, kSha256DigestPrefix.size()) != kSha256DigestPrefix) {
    return false;
  }
  for (const char digit : value.substr(kSha256DigestPrefix.size())) {
    if (!((digit >= '0' && digit <= '9') ||
          (digit >= 'a' && digit <= 'f'))) {
      return false;
    }
  }
  return true;
}

/// Exact logical-scene identity. The scene owner alone mints monotonically
/// increasing revision IDs and the deterministic logical-content hash.
struct SceneRevision final {
  SceneId scene_id{std::string{}};
  std::uint64_t revision_id = 0U;
  ContentDigest content_hash{std::string{}};

  [[nodiscard]] bool valid() const noexcept {
    return revision_id != 0U &&
           IsExactLowercaseSha256Digest(scene_id.value()) &&
           IsExactLowercaseSha256Digest(content_hash.value());
  }

  friend bool operator==(const SceneRevision&, const SceneRevision&) = default;
};

/// Indivisible retained-bundle, graph, and logical-scene projection identity.
struct ProjectionRevisionDTO final {
  RetainedBundleId retained_bundle_id{std::string{}};
  GraphRevision graph_revision{std::string{}};
  SceneRevision scene_revision;

  [[nodiscard]] bool valid() const noexcept {
    return !retained_bundle_id.empty() && !graph_revision.empty() &&
           scene_revision.valid();
  }

  friend bool operator==(const ProjectionRevisionDTO&,
                         const ProjectionRevisionDTO&) = default;
};

struct ProjectionLeaseBindingDTO final {
  ProjectionRevisionDTO projection_revision;
  ProjectionLeaseId projection_lease_id{std::string{}};

  [[nodiscard]] bool valid() const noexcept {
    return projection_revision.valid() && !projection_lease_id.empty();
  }

  friend bool operator==(const ProjectionLeaseBindingDTO&,
                         const ProjectionLeaseBindingDTO&) = default;
};

/// One exact native planner/collision projection process incarnation.
struct ProjectionNativeRuntimeBindingDTO final {
  CapabilityProviderBindingId provider_binding_id;
  CapabilityProviderBindingRevision provider_binding_revision;
  RuntimeUnitId runtime_unit_id;
  RuntimeEpoch runtime_epoch;

  [[nodiscard]] bool valid() const noexcept {
    return !provider_binding_id.empty() && !provider_binding_revision.empty() &&
           !runtime_unit_id.empty() && !runtime_epoch.empty();
  }

  friend bool operator==(const ProjectionNativeRuntimeBindingDTO&,
                         const ProjectionNativeRuntimeBindingDTO&) = default;
};

/// Canonical native-binding order required by specs 210 and 220.
[[nodiscard]] inline bool CanonicalProjectionNativeBindingLess(
    const ProjectionNativeRuntimeBindingDTO& lhs,
    const ProjectionNativeRuntimeBindingDTO& rhs) noexcept {
  if (lhs.provider_binding_id != rhs.provider_binding_id) {
    return lhs.provider_binding_id < rhs.provider_binding_id;
  }
  if (lhs.provider_binding_revision != rhs.provider_binding_revision) {
    return lhs.provider_binding_revision < rhs.provider_binding_revision;
  }
  if (lhs.runtime_unit_id != rhs.runtime_unit_id) {
    return lhs.runtime_unit_id < rhs.runtime_unit_id;
  }
  return lhs.runtime_epoch < rhs.runtime_epoch;
}

enum class ProjectionHealthState {
  kUninitialized,
  kHealthy,
  kDesynchronized,
  kRebuilding,
  kUnavailable,
};

[[nodiscard]] constexpr std::string_view
to_string(ProjectionHealthState state) noexcept {
  switch (state) {
    case ProjectionHealthState::kUninitialized:
      return "uninitialized";
    case ProjectionHealthState::kHealthy:
      return "healthy";
    case ProjectionHealthState::kDesynchronized:
      return "desynchronized";
    case ProjectionHealthState::kRebuilding:
      return "rebuilding";
    case ProjectionHealthState::kUnavailable:
      return "unavailable";
  }
  return {};
}

[[nodiscard]] constexpr bool IsKnownProjectionHealthState(
    ProjectionHealthState state) noexcept {
  switch (state) {
    case ProjectionHealthState::kUninitialized:
    case ProjectionHealthState::kHealthy:
    case ProjectionHealthState::kDesynchronized:
    case ProjectionHealthState::kRebuilding:
    case ProjectionHealthState::kUnavailable:
      return true;
  }
  return false;
}

/// The staged fields are one atomic optional tuple. Keeping them in one value
/// prevents partial presence from being representable.
struct ProjectionStagedCandidateDTO final {
  ApplyId apply_id;
  ApplyGeneration apply_generation;
  ProjectionStageGeneration projection_stage_generation;
  ProjectionRevisionDTO candidate_projection_revision;

  [[nodiscard]] bool valid() const noexcept {
    return !apply_id.empty() && apply_generation.nonzero() &&
           projection_stage_generation.nonzero() &&
           candidate_projection_revision.valid();
  }

  friend bool operator==(const ProjectionStagedCandidateDTO&,
                         const ProjectionStagedCandidateDTO&) = default;
};

struct ProjectionHealthDTO final {
  ProjectionNativeRuntimeBindingDTO native_runtime_binding;
  std::uint64_t source_sequence;
  ProjectionHealthState state;
  std::optional<ProjectionRevisionDTO> active_projection_revision;
  std::optional<ProjectionStagedCandidateDTO> staged_candidate;
  std::optional<std::uint64_t> native_projection_generation;
  std::optional<ContentDigest> projection_content_digest;
  std::optional<ErrorDTO> error;

  [[nodiscard]] bool valid() const noexcept {
    return native_runtime_binding.valid() &&
           IsKnownProjectionHealthState(state) &&
           (!active_projection_revision.has_value() ||
            active_projection_revision->valid()) &&
           (!staged_candidate.has_value() || staged_candidate->valid()) &&
           (!native_projection_generation.has_value() ||
            *native_projection_generation != 0U) &&
           (!projection_content_digest.has_value() ||
            !projection_content_digest->empty());
  }

  /// Locally proves that this observed row is cleanly healthy at `target`.
  ///
  /// Required-set membership and freshness remain contextual scene-owner
  /// checks; this predicate deliberately proves neither.
  [[nodiscard]] bool healthy_for_target(
      const ProjectionRevisionDTO& target) const noexcept {
    return valid() && target.valid() &&
           state == ProjectionHealthState::kHealthy && !error.has_value() &&
           active_projection_revision.has_value() &&
           *active_projection_revision == target &&
           !staged_candidate.has_value();
  }
};

struct NamedApplyProjectionSetSourceDTO final {
  ApplyId apply_id;
  ApplyGeneration apply_generation;
  ProjectionStageGeneration projection_stage_generation;
  ProjectionRevisionDTO candidate_projection_revision;

  [[nodiscard]] bool valid() const noexcept {
    return !apply_id.empty() && apply_generation.nonzero() &&
           projection_stage_generation.nonzero() &&
           candidate_projection_revision.valid();
  }

  friend bool operator==(const NamedApplyProjectionSetSourceDTO&,
                         const NamedApplyProjectionSetSourceDTO&) = default;
};

struct BootstrapProjectionSetSourceDTO final {
  BootstrapId bootstrap_id;
  BootstrapGeneration bootstrap_generation;
  ProjectionRevisionDTO canonical_empty_target;

  [[nodiscard]] bool valid() const noexcept {
    return !bootstrap_id.empty() && bootstrap_generation.nonzero() &&
           canonical_empty_target.valid();
  }

  friend bool operator==(const BootstrapProjectionSetSourceDTO&,
                         const BootstrapProjectionSetSourceDTO&) = default;
};

using RequiredNativeProjectionSetSourceDTO =
    std::variant<NamedApplyProjectionSetSourceDTO,
                 BootstrapProjectionSetSourceDTO>;

struct ProjectionAggregateHealthDTO final {
  RequiredNativeProjectionSetSourceDTO required_native_set_source;
  ProjectionHealthState state;
  std::vector<ProjectionHealthDTO> native_projection_health;
  std::optional<ErrorDTO> error;

  [[nodiscard]] bool valid() const noexcept {
    const bool source_valid = std::visit(
        [](const auto& source) { return source.valid(); },
        required_native_set_source);
    if (!source_valid || !IsKnownProjectionHealthState(state)) {
      return false;
    }
    for (std::size_t index = 0U; index < native_projection_health.size();
         ++index) {
      const auto& health = native_projection_health[index];
      if (!health.valid()) {
        return false;
      }
      if (index != 0U &&
          !CanonicalProjectionNativeBindingLess(
              native_projection_health[index - 1U].native_runtime_binding,
              health.native_runtime_binding)) {
        return false;
      }
    }
    return true;
  }

  /// Locally proves that all supplied canonical rows are healthy at `target`.
  ///
  /// The exact required native set belongs to the persisted Apply/bootstrap
  /// context. This predicate does not claim that the supplied rows are
  /// complete for that set.
  [[nodiscard]] bool healthy_for_target(
      const ProjectionRevisionDTO& target) const noexcept {
    if (!valid() || !target.valid() ||
        state != ProjectionHealthState::kHealthy || error.has_value()) {
      return false;
    }
    for (const auto& health : native_projection_health) {
      if (!health.healthy_for_target(target)) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] bool matches_named_apply(
      const ApplyId& apply_id, ApplyGeneration apply_generation,
      ProjectionStageGeneration stage_generation,
      const ProjectionRevisionDTO& candidate_revision) const noexcept {
    const auto* source =
        std::get_if<NamedApplyProjectionSetSourceDTO>(
            &required_native_set_source);
    return valid() && source != nullptr && source->apply_id == apply_id &&
           source->apply_generation == apply_generation &&
           source->projection_stage_generation == stage_generation &&
           source->candidate_projection_revision == candidate_revision;
  }

  [[nodiscard]] bool matches_bootstrap(
      const BootstrapId& bootstrap_id,
      BootstrapGeneration bootstrap_generation,
      const ProjectionRevisionDTO& target) const noexcept {
    const auto* source =
        std::get_if<BootstrapProjectionSetSourceDTO>(
            &required_native_set_source);
    return valid() && source != nullptr &&
           source->bootstrap_id == bootstrap_id &&
           source->bootstrap_generation == bootstrap_generation &&
           source->canonical_empty_target == target;
  }
};

/// Compares only canonical native-binding membership.
///
/// Source sequences and health payloads may differ between observations. Both
/// aggregates still must be locally valid and canonical before their binding
/// sets can be compared.
[[nodiscard]] inline bool EqualCanonicalProjectionHealthBindings(
    const ProjectionAggregateHealthDTO& lhs,
    const ProjectionAggregateHealthDTO& rhs) noexcept {
  if (!lhs.valid() || !rhs.valid() ||
      lhs.native_projection_health.size() !=
          rhs.native_projection_health.size()) {
    return false;
  }
  for (std::size_t index = 0U;
       index < lhs.native_projection_health.size(); ++index) {
    if (lhs.native_projection_health[index].native_runtime_binding !=
        rhs.native_projection_health[index].native_runtime_binding) {
      return false;
    }
  }
  return true;
}

}  // namespace manyforge::core
