#pragma once

#include <manyforge_core/addressed_observed_state_publication.hpp>
#include <manyforge_core/ports/iobserved_state_provider.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <memory>
#include <vector>

namespace manyforge::core {

/// Construction-issued continuous measured source. Its complete query and
/// source identities are distinct from the commanded execution/Stop channel.
/// No default authority, source identity, coordinate list or age budget is
/// supplied here. A route replacement constructs a fresh empty publication.
struct MeasuredStateSourceDescriptor final {
  AddressedObservedStatePublicationConfig publication;
  /// Qualified actual ROS names, index-aligned with the exact query resource
  /// and coordinate order. Compiled names need not equal transport names.
  std::vector<std::string> source_joint_names;

  [[nodiscard]] Result<void> validate() const;
};

/// One required geometry-coordinate source, including an unavailable source.
/// The owning reader is deliberately nullable: missing runtime handles must
/// not remove a configured robot from the immutable inventory.
struct RequiredRobotMeasuredStateSource final {
  MeasuredStateSourceDescriptor descriptor;
  std::shared_ptr<const IObservedStateProvider> reader;
};

/// The resource list comes from the complete verified robot geometry, rather
/// than the operation's selected participant or currently available handles.
struct ConfiguredRobotMeasuredStateRequirement final {
  ControllableComponentId component_id;
  std::vector<ResourceId> required_resources;
  std::vector<RequiredRobotMeasuredStateSource> sources;
};

/// Owning source leases captured by the existing product's route publisher.
/// The publisher authenticates their exact current route/provider association
/// and rechecks that association after reading. This value does not select
/// membership or grant source authority; the inventory retains the complete
/// configured geometry requirement independently of available readers.
struct RobotMeasuredStateSourceView final {
  ControllableComponentId component_id;
  std::vector<RequiredRobotMeasuredStateSource> sources;
};

struct RobotMeasuredStateCapture final {
  ControllableComponentId component_id;
  std::vector<ObservedStateSnapshotDTO> sources;
};

struct ConfiguredRobotMeasuredStateCapture final {
  std::int64_t evaluated_at_ns{};
  std::vector<RobotMeasuredStateCapture> robots;
};

/// A single immutable product-owned inventory. Construction preserves every
/// declared required row, even an unavailable one; reads never discover or
/// mutate membership. The owner must derive those rows from the actual
/// verified product. This value cannot authenticate a caller's membership.
class ConfiguredRobotMeasuredStateInventory final {
 public:
  explicit ConfiguredRobotMeasuredStateInventory(
      std::vector<ConfiguredRobotMeasuredStateRequirement> robots);

  [[nodiscard]] const std::vector<ConfiguredRobotMeasuredStateRequirement>& robots() const noexcept {
    return robots_;
  }

  /// Pure owner read at one injected receiver-monotonic time. No effects,
  /// clock reads, re-timestamps, retries, lifecycle checks or state changes.
  [[nodiscard]] Result<ConfiguredRobotMeasuredStateCapture> Read(std::int64_t now_ns) const;

  /// Read exactly the known configured requirement through an explicit owning
  /// route-source view. No static-source fallback, discovery, clock read or
  /// state change; success says nothing about any other configured robot.
  [[nodiscard]] Result<RobotMeasuredStateCapture> ReadComponent(
      const RobotMeasuredStateSourceView& source_view, std::int64_t now_ns) const;

 private:
  [[nodiscard]] Result<RobotMeasuredStateCapture> ReadRequirement(
      const ConfiguredRobotMeasuredStateRequirement& robot,
      const std::vector<RequiredRobotMeasuredStateSource>& sources,
      std::int64_t now_ns) const;

  const std::vector<ConfiguredRobotMeasuredStateRequirement> robots_;
};

}  // namespace manyforge::core
