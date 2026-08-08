#pragma once

#include <atomic>
#include <cstdint>
#include <manyforge_core/observed_robot_state_publication.hpp>
#include <manyforge_core/ports/iobserved_robot_state_provider.hpp>
#include <manyforge_core/ports/iobserved_state_provider.hpp>
#include <memory>
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

  [[nodiscard]] const ObservedStateQueryContextDTO& query_context() const noexcept {
    return config_.query_context;
  }
  [[nodiscard]] std::uint64_t authority_generation() const noexcept {
    return config_.authority_generation;
  }
  [[nodiscard]] const ObservedRobotStatePublication& numeric_publication() const noexcept {
    return publication_;
  }

 private:
  [[nodiscard]] ObservedStateSnapshotDTO make_snapshot(
      const ObservedRobotStatePublication::ReadView& view) const;

  AddressedObservedStatePublicationConfig config_;
  ObservedRobotStatePublication publication_;
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
