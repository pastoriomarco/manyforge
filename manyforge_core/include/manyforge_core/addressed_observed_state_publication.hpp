#pragma once

#include <atomic>
#include <cstdint>
#include <manyforge_core/observed_robot_state_publication.hpp>
#include <manyforge_core/ports/iobserved_robot_state_provider.hpp>
#include <manyforge_core/ports/iobserved_state_provider.hpp>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <vector>

namespace manyforge::core {

struct AddressedObservedStatePublicationConfig final {
  ObservedStateQueryContextDTO query_context;
  std::vector<ObservedRobotStateSourceConfig> source_precedence;
  std::uint64_t authority_generation{1U};
  std::size_t maximum_source_resource_count{256U};
};

/// Addressed, unit-tagged facade over the already-qualified bounded native
/// publication channel. Raw doubles are interpreted only through the exact
/// retained coordinate-kind vector captured at construction.
class AddressedObservedStatePublication final : public IObservedStateProvider {
 public:
  explicit AddressedObservedStatePublication(AddressedObservedStatePublicationConfig config);

  AddressedObservedStatePublication(const AddressedObservedStatePublication&) = delete;
  AddressedObservedStatePublication& operator=(const AddressedObservedStatePublication&) = delete;

  [[nodiscard]] ObservedRobotStatePublishResult publish(
      ObservedStateSourceKind source, std::span<const std::string> source_joint_names,
      std::span<const double> positions_si, std::span<const double> velocities_si,
      std::int64_t source_timestamp_ns, std::int64_t received_timestamp_ns) noexcept;

  [[nodiscard]] Result<ObservedStateSnapshotDTO> current_state(
      const ObservedStateQueryContextDTO& context, std::int64_t now_ns) const override;

  [[nodiscard]] Result<ObservedStatePublicationDTO> publication(
      const ControlBindingId& control_binding_id,
      const ControlBindingRevision& control_binding_revision) const override;
  [[nodiscard]] Result<ObservedStatePublicationDTO> publication_at(
      const ControlBindingId& control_binding_id,
      const ControlBindingRevision& control_binding_revision, std::int64_t now_ns) const override;

  /// Rebind only the route/configuration identity of a commanded or predicted
  /// source while retaining its exact physical source and numeric channel.
  /// The latest complete owner sample is republished at `transition_time_ns`
  /// before the new context becomes visible. Measured authorities must instead
  /// publish a fresh measurement through their owning adapter.
  [[nodiscard]] Result<void> rebind_route_context(ObservedStateQueryContextDTO query_context,
                                                  std::int64_t transition_time_ns);

  /// NRT value copy. A route replacement never invalidates a caller's context.
  [[nodiscard]] ObservedStateQueryContextDTO query_context() const;
  [[nodiscard]] std::uint64_t authority_generation() const noexcept {
    return config_.authority_generation;
  }
  [[nodiscard]] const ObservedRobotStatePublication& numeric_publication() const noexcept {
    return publication_;
  }
  /// Single-writer construction seam for a retained execution adapter. The
  /// addressed owner keeps storage and query authority; callers may only
  /// publish source kinds already sealed into this instance.
  [[nodiscard]] ObservedRobotStatePublication& numeric_publication_writer() noexcept {
    return publication_;
  }

 private:
  friend struct AddressedObservedStatePublicationTestAccess;
  [[nodiscard]] ObservedStateSnapshotDTO make_snapshot(
      const ObservedRobotStatePublication::ReadView& view,
      const ObservedStateQueryContextDTO& context) const;
  [[nodiscard]] ObservedStatePublicationDTO make_publication(
      const ObservedRobotStatePublication::ReadView& view,
      const ObservedStateQueryContextDTO& context) const;

  AddressedObservedStatePublicationConfig config_;
  ObservedRobotStatePublication publication_;
  // Readers share this lock and never wait: they only refuse while a route
  // rebind holds it exclusively, so ordinary readers cannot fail each other.
  mutable std::shared_mutex route_context_mutex_;
  ObservedStateQueryContextDTO current_query_context_;
  std::uint64_t context_first_sequence_{};
};

/// Explicit compatibility adapter for the existing all-angular subordinate
/// execution engine. Construction fails for a mixed or prismatic target, so
/// metres can never enter RobotStateDTO's historical angular fields.
class AddressedAngularObservedRobotStateAdapter final : public IObservedRobotStateProvider {
 public:
  explicit AddressedAngularObservedRobotStateAdapter(
      const AddressedObservedStatePublication& publication);

  [[nodiscard]] Result<RobotStateDTO> current_state(std::int64_t now_ns) const override;
  [[nodiscard]] const ObservedRobotStatePublication* publication() const noexcept override;
  [[nodiscard]] Result<ObservedStatePublicationDTO> addressed_publication(
      const ControlBindingId& control_binding_id,
      const ControlBindingRevision& control_binding_revision) const override;

 private:
  const AddressedObservedStatePublication* publication_;
};

}  // namespace manyforge::core
