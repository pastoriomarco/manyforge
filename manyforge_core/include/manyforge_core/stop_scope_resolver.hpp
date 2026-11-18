#pragma once

#include <cstdint>
#include <manyforge_core/types/operation_runtime.hpp>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace manyforge::core {

/// Current commandable-resource membership of one semantic component (the
/// Kernel's component scope, sorted by component id).
struct StopScopeComponentSnapshotDTO final {
  ControllableComponentId controllable_component_id;
  ComponentLifecycleGeneration component_lifecycle_generation;
  std::vector<ResourceId> resources;

  friend bool operator==(const StopScopeComponentSnapshotDTO&,
                         const StopScopeComponentSnapshotDTO&) = default;
};

/// Stop target resolution (stop state model §3.2 S4, §5.4). Pure,
/// allocation-free: the Kernel calls it under its lock while it walks its own
/// resident operations, so no snapshot is built.
enum class StopTargetResolution : std::uint8_t {
  /// The target names current scope with current metadata.
  kExact = 0,
  /// The target's expected operation revision is old: its current scope is
  /// stopped and a stale diagnostic is reported.
  kStaleMetadata,
  /// The target is known and terminal: nothing to contain for it.
  kNoEffect,
  /// The target names nothing known: the request is invalid (PD-1 widening).
  kUnknown,
};

/// The field that makes a parsed Stop intent unusable as a normalized
/// request (its reason text excepted), or nullopt. Such a request is widened
/// (`StopWidenReason::kMalformed`). Called by the Kernel entries before the
/// Kernel lock is taken.
[[nodiscard]] std::optional<std::string_view> NormalizedStopIntentFieldError(
    const StopIntentDTO& intent);

/// An operation needs containment when it is not terminal, or terminal with
/// an uncertain effect (`requires_containment`, unchanged).
[[nodiscard]] bool stop_requires_containment(OperationStateKind state,
                                             bool effect_uncertain) noexcept;

/// One operation target against the operation it names: `current_revision`
/// is the resident operation's revision (absent when not resident),
/// `known_historical` whether it is in the retained history.
[[nodiscard]] StopTargetResolution classify_stop_operation_target(
    const StopOperationTargetDTO& target, std::optional<OperationRevision> current_revision,
    bool requires_containment, bool known_historical) noexcept;

/// The component in the Kernel's sorted component scope, or null.
[[nodiscard]] const StopScopeComponentSnapshotDTO* find_stop_component(
    std::span<const StopScopeComponentSnapshotDTO> components,
    const ControllableComponentId& component) noexcept;

/// The component of the applied cell that owns `resource`, or null (a
/// resource target naming no commandable resource is unknown).
[[nodiscard]] const StopScopeComponentSnapshotDTO* find_stop_resource_owner(
    std::span<const StopScopeComponentSnapshotDTO> components, const ResourceId& resource) noexcept;

}  // namespace manyforge::core
