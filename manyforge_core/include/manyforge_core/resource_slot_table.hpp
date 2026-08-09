#pragma once

#include <cstddef>
#include <cstdint>
#include <manyforge_core/types/cell_graph.hpp>
#include <manyforge_core/types/configuration_fence.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace manyforge::core {

inline constexpr std::size_t kMaxKernelResourceSlots = 4096U;

enum class ResourceSlotState {
  kFree,
  kReserved,
  kDispatched,
  kStopping,
  kTerminal,
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
  ContainmentGranularity containment_granularity{ContainmentGranularity::kResource};

  [[nodiscard]] bool valid() const noexcept;

  friend bool operator==(const ResourceSlotOwnerDTO&, const ResourceSlotOwnerDTO&) = default;
};

struct ResourceSlotSnapshotDTO final {
  ResourceId resource_id;
  ResourceSlotState state{ResourceSlotState::kFree};
  std::optional<ResourceSlotOwnerDTO> owner;

  friend bool operator==(const ResourceSlotSnapshotDTO&, const ResourceSlotSnapshotDTO&) = default;
};

/// One atomic read of the operation/resource truth owned by a slot table.
/// Counts are operation counts, not resource counts. Reserved operations are
/// queued; dispatched, stopping, terminal-awaiting-acknowledgement, and
/// unreconciled orphan work are active for Apply-admission purposes.
struct ResourceSlotAdmissionSnapshotDTO final {
  std::uint64_t operation_resource_generation{};
  std::vector<DomainActiveControlConfigurationDTO> active_control_configurations;
  std::uint64_t affected_queued_operation_count{};
  std::uint64_t affected_active_operation_count{};
};

/// Fixed-inventory I6 Kernel reservation table.
///
/// I6 deliberately admits only one operation at a time while proving final
/// resource identities. I8 can relax that admission rule without changing
/// slot identity or owner evidence. Every mutation is atomic over the supplied
/// resource set and no method performs I/O or waits. A configuration fence is
/// not a second slot state: the injected admission gate serializes it with the
/// real reservation while retaining the durable fence identity separately.
class ResourceSlotTable final {
 public:
  explicit ResourceSlotTable(
      std::vector<ResourceId> resources,
      std::vector<DomainActiveControlConfigurationDTO> active_control_configurations = {},
      std::shared_ptr<std::recursive_mutex> admission_mutex = {});

  ResourceSlotTable(const ResourceSlotTable&) = delete;
  ResourceSlotTable& operator=(const ResourceSlotTable&) = delete;

  [[nodiscard]] Result<void> reserve(const ResourceSlotOwnerDTO& owner,
                                     const std::vector<ResourceId>& resources);

  [[nodiscard]] Result<void> transition(const ResourceSlotOwnerDTO& owner,
                                        const std::vector<ResourceId>& resources,
                                        ResourceSlotState next_state);

  /// Advance the owning operation revision after an accepted operation-scoped
  /// control without changing reservation state or resolved runtime facts.
  [[nodiscard]] Result<ResourceSlotOwnerDTO> revise_owner(
      const ResourceSlotOwnerDTO& owner, const std::vector<ResourceId>& resources,
      OperationRevision next_operation_revision);

  /// Atomically advance slot state and owning operation revision. This is the
  /// acceptance primitive for controls such as scoped Stop whose state and
  /// CAS fence must never be observably split.
  [[nodiscard]] Result<ResourceSlotOwnerDTO> transition_and_revise_owner(
      const ResourceSlotOwnerDTO& owner, const std::vector<ResourceId>& resources,
      ResourceSlotState next_state, OperationRevision next_operation_revision);

  /// Clear an exact pre-dispatch reservation or acknowledged terminal scope.
  [[nodiscard]] Result<void> release(const ResourceSlotOwnerDTO& owner,
                                     const std::vector<ResourceId>& resources);

  /// Mark free resources conservatively unavailable after Kernel replacement.
  /// No prior operation owner is recreated and this performs no controller or
  /// transport effect. Later recovery must prove fresh quiescence before an
  /// explicit reconciliation surface may release the marks.
  [[nodiscard]] Result<void> mark_unreconciled_external_or_orphan_goal(
      const std::vector<ResourceId>& resources);

  /// Replace the selected runtime inventory while preserving every unchanged
  /// occupied slot. An empty candidate is the valid no-commandable-component
  /// product. Removed resources must already be free, and any retained
  /// operation must still name the exact active control configuration selected
  /// by the candidate runtime. The caller is responsible for holding the
  /// configuration fence that covers every changed resource.
  [[nodiscard]] Result<void> reconfigure_runtime_inventory(
      std::vector<ResourceId> resources,
      std::vector<DomainActiveControlConfigurationDTO> active_control_configurations);

  [[nodiscard]] std::vector<ResourceSlotSnapshotDTO> snapshot() const;
  [[nodiscard]] ResourceSlotAdmissionSnapshotDTO admission_snapshot(
      const std::vector<ResourceId>& affected_resources) const;
  [[nodiscard]] std::optional<OperationId> active_operation_id() const;
  [[nodiscard]] bool shares_admission_mutex(
      const std::shared_ptr<std::recursive_mutex>& admission_mutex) const noexcept;

 private:
  [[nodiscard]] Result<void> validate_resource_set_locked(const std::vector<ResourceId>& resources,
                                                          std::vector<std::size_t>& indices) const;
  void clear_active_operation_if_unused_locked();
  [[nodiscard]] Result<void> require_generation_available_locked() const;
  void advance_generation_locked() noexcept;

  std::shared_ptr<std::recursive_mutex> admission_mutex_;
  std::vector<ResourceSlotSnapshotDTO> slots_;
  std::vector<DomainActiveControlConfigurationDTO> active_control_configurations_;
  std::optional<OperationId> active_operation_id_;
  std::uint64_t operation_resource_generation_{};
};

/// Optional process-local interlock used by a Kernel reservation path. The
/// implementation owns no resource counters: it serializes the table's real
/// reservation with other admission effects such as a durable configuration
/// fence. It is not caller authorization or a lease/session check; a valid
/// addressed command is blocked only when its resources overlap a held fence.
class IResourceOperationAdmissionGate {
 public:
  virtual ~IResourceOperationAdmissionGate() = default;

  [[nodiscard]] virtual std::shared_ptr<std::recursive_mutex> shared_admission_mutex() const = 0;
  [[nodiscard]] virtual Result<void> reserve(ResourceSlotTable& table,
                                             const ResourceSlotOwnerDTO& owner,
                                             const std::vector<ResourceId>& resources) = 0;
};

}  // namespace manyforge::core
