#pragma once

#include <cstddef>
#include <mutex>
#include <optional>
#include <vector>

#include <manyforge_core/types/cell_graph.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/strong_id.hpp>

namespace manyforge::core {

inline constexpr std::size_t kMaxKernelResourceSlots = 4096U;

enum class ResourceSlotState {
  kFree,
  kReserved,
  kDispatched,
  kStopping,
  kTerminal,
  kFenced,
  kUnreconciledExternalOrOrphanGoal,
};

struct ResourceSlotOwnerDTO final {
  OperationId operation_id;
  OperationRevision operation_revision;
  OperationParticipantId participant_id;
  ControlDomainId control_domain_id;
  ControlDomainRevision control_domain_revision;
  ControlBindingId control_binding_id;
  ControlBindingRevision control_binding_revision;
  ControllerTopologyId controller_topology_id;
  TopologyRevision topology_revision;
  ControlRouteId control_route_id;
  ActiveControlConfigurationId active_control_configuration_id;
  ActiveControlConfigurationGeneration active_control_configuration_generation;
  ContainmentGranularity containment_granularity{
      ContainmentGranularity::kResource};

  [[nodiscard]] bool valid() const noexcept;

  friend bool operator==(const ResourceSlotOwnerDTO&,
                         const ResourceSlotOwnerDTO&) = default;
};

struct ResourceSlotSnapshotDTO final {
  ResourceId resource_id;
  ResourceSlotState state{ResourceSlotState::kFree};
  std::optional<ResourceSlotOwnerDTO> owner;

  friend bool operator==(const ResourceSlotSnapshotDTO&,
                         const ResourceSlotSnapshotDTO&) = default;
};

/// Fixed-inventory I6 Kernel reservation table.
///
/// I6 deliberately admits only one operation at a time while proving final
/// resource identities. I8 can relax that admission rule without changing
/// slot identity or owner evidence. Every mutation is atomic over the supplied
/// resource set and no method performs I/O or waits.
class ResourceSlotTable final {
 public:
  explicit ResourceSlotTable(std::vector<ResourceId> resources);

  ResourceSlotTable(const ResourceSlotTable&) = delete;
  ResourceSlotTable& operator=(const ResourceSlotTable&) = delete;

  [[nodiscard]] Result<void> reserve(
      const ResourceSlotOwnerDTO& owner,
      const std::vector<ResourceId>& resources);

  [[nodiscard]] Result<void> transition(
      const ResourceSlotOwnerDTO& owner,
      const std::vector<ResourceId>& resources,
      ResourceSlotState next_state);

  /// Advance the owning operation revision after an accepted operation-scoped
  /// control without changing reservation state or resolved runtime facts.
  [[nodiscard]] Result<ResourceSlotOwnerDTO> revise_owner(
      const ResourceSlotOwnerDTO& owner,
      const std::vector<ResourceId>& resources,
      OperationRevision next_operation_revision);

  /// Clear an exact pre-dispatch reservation or acknowledged terminal scope.
  [[nodiscard]] Result<void> release(
      const ResourceSlotOwnerDTO& owner,
      const std::vector<ResourceId>& resources);

  /// Administrative fences are admitted only on free slots. Active work must
  /// first settle through the owning Kernel operation.
  [[nodiscard]] Result<void> fence(
      const std::vector<ResourceId>& resources);
  [[nodiscard]] Result<void> unfence(
      const std::vector<ResourceId>& resources);

  [[nodiscard]] std::vector<ResourceSlotSnapshotDTO> snapshot() const;
  [[nodiscard]] std::optional<OperationId> active_operation_id() const;

 private:
  [[nodiscard]] Result<void> validate_resource_set_locked(
      const std::vector<ResourceId>& resources,
      std::vector<std::size_t>& indices) const;
  void clear_active_operation_if_unused_locked();

  mutable std::mutex mutex_;
  std::vector<ResourceSlotSnapshotDTO> slots_;
  std::optional<OperationId> active_operation_id_;
};

}  // namespace manyforge::core
