#pragma once

#include <cstddef>
#include <cstdint>
#include <manyforge_core/types/cell_graph.hpp>
#include <manyforge_core/types/configuration_fence.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/participant_binding_identity.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
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

struct ResourceSlotControllerAuthorityDTO final {
  ControllerTopologyId controller_topology_id;
  TopologyRevision topology_revision;
  ControlRouteId control_route_id;
  ActiveControlConfigurationId active_control_configuration_id;
  ActiveControlConfigurationGeneration active_control_configuration_generation;
  ContainmentGranularity containment_granularity{ContainmentGranularity::kResource};
  friend bool operator==(const ResourceSlotControllerAuthorityDTO&,
                         const ResourceSlotControllerAuthorityDTO&) = default;
};

struct ResourceSlotOwnerDTO final {
  OperationId operation_id;
  OperationRevision operation_revision;
  OperationParticipantId participant_id;
  ParticipantBindingIdentityDTO binding_identity;
  ResourceSlotControllerAuthorityDTO controller_authority;

  ResourceSlotOwnerDTO() = default;
  ResourceSlotOwnerDTO(OperationId operation, OperationRevision revision,
                       OperationParticipantId participant, ControlDomainId domain,
                       ControlDomainRevision domain_revision, ControlBindingId binding,
                       ControlBindingRevision binding_revision, ControllerTopologyId topology,
                       TopologyRevision topology_revision, ControlRouteId route,
                       ActiveControlConfigurationId configuration,
                       ActiveControlConfigurationGeneration configuration_generation,
                       ContainmentGranularity granularity = ContainmentGranularity::kResource)
      : operation_id(std::move(operation)),
        operation_revision(revision),
        participant_id(std::move(participant)),
        binding_identity{std::move(domain), std::move(domain_revision),
                         ControllerParticipantBindingIdentityDTO{
                             std::move(binding), std::move(binding_revision), {}}},
        controller_authority{std::move(topology), std::move(topology_revision), std::move(route),
                             std::move(configuration), configuration_generation, granularity} {}
  [[nodiscard]] ResourceSlotControllerAuthorityDTO& controller() { return controller_authority; }
  [[nodiscard]] const ResourceSlotControllerAuthorityDTO& controller() const {
    return controller_authority;
  }
  [[nodiscard]] bool valid() const noexcept;
  friend bool operator==(const ResourceSlotOwnerDTO&, const ResourceSlotOwnerDTO&) = default;
};

struct ResourceSlotSnapshotDTO final {
  ResourceId resource_id;
  ResourceSlotState state{ResourceSlotState::kFree};
  std::optional<ResourceSlotOwnerDTO> owner;
  /// Latest Kernel Stop generation that fenced this resource. It remains
  /// monotonic across terminal acknowledgement/reuse so resource-addressed
  /// Stop expectations never regain an older value accidentally.
  StopGeneration stop_generation{0U};

  friend bool operator==(const ResourceSlotSnapshotDTO&, const ResourceSlotSnapshotDTO&) = default;
};

/// One coherent read of every resource slot and the generation that identifies
/// those exact bytes. Stop resolution consumes this value rather than mixing a
/// slot snapshot with a separately sampled admission generation.
struct ResourceSlotTableSnapshotDTO final {
  std::uint64_t operation_resource_generation{};
  std::vector<ResourceSlotSnapshotDTO> slots;

  friend bool operator==(const ResourceSlotTableSnapshotDTO&,
                         const ResourceSlotTableSnapshotDTO&) = default;
};

/// One participant owner in an atomic coordinated-operation reservation.
/// Rows form a canonical keyed set ordered by (operation_id, participant_id).
/// Each resource scope is non-empty, sorted, and unique; scopes are disjoint
/// across the complete batch.
struct ResourceSlotReservationOwnerDTO final {
  ResourceSlotOwnerDTO owner;
  std::vector<ResourceId> resources;

  friend bool operator==(const ResourceSlotReservationOwnerDTO&,
                         const ResourceSlotReservationOwnerDTO&) = default;
};

enum class ResourceSlotOwnerTransferMode : std::uint8_t {
  kMoving,
  kSettled,
};

/// One complete incumbent participant scope transferred to a fresh operation.
/// Rows are canonical by (expected operation_id, participant_id); each resource
/// scope is sorted/unique and disjoint. Only operation/participant identity and
/// operation revision may change; every physical binding/configuration stays exact.
struct ResourceSlotOwnerTransferDTO final {
  ResourceSlotOwnerDTO expected_owner;
  ResourceSlotOwnerDTO next_owner;
  std::vector<ResourceId> resources;

  friend bool operator==(const ResourceSlotOwnerTransferDTO&,
                         const ResourceSlotOwnerTransferDTO&) = default;
};

/// One exact participant owner in an atomic multi-owner Stop fence. Rows are a
/// canonical keyed set ordered by (operation_id, participant_id); resources are
/// non-empty, sorted, unique, and disjoint across the complete request.
struct ResourceSlotStopFenceOwnerDTO final {
  ResourceSlotOwnerDTO expected_owner;
  std::vector<ResourceId> resources;
  OperationRevision next_operation_revision;

  friend bool operator==(const ResourceSlotStopFenceOwnerDTO&,
                         const ResourceSlotStopFenceOwnerDTO&) = default;
};

/// Canonical bounded all-or-nothing Stop fence. Orphan resources name retained
/// external/effect-uncertain slots that are already admission-closed but must
/// be stamped by the same Stop generation as the owned scopes.
struct ResourceSlotStopFenceRequestDTO final {
  std::uint64_t expected_operation_resource_generation{};
  StopGeneration stop_generation{0U};
  std::vector<ResourceSlotStopFenceOwnerDTO> owners;
  std::vector<ResourceId> orphan_resources;
  /// Exact non-fenced owners of revised operations. Preserve phase and Stop generation.
  std::vector<ResourceSlotStopFenceOwnerDTO> revision_only_owners{};

  friend bool operator==(const ResourceSlotStopFenceRequestDTO&,
                         const ResourceSlotStopFenceRequestDTO&) = default;
};

struct ResourceSlotStopFenceResultDTO final {
  std::uint64_t operation_resource_generation{};
  StopGeneration stop_generation{0U};
  std::vector<ResourceSlotOwnerDTO> revised_owners;
  std::vector<ResourceId> orphan_resources;

  friend bool operator==(const ResourceSlotStopFenceResultDTO&,
                         const ResourceSlotStopFenceResultDTO&) = default;
};

/// One atomic read of the operation/resource truth owned by a slot table.
/// Counts are operation counts, not resource counts. Reserved operations are
/// queued; dispatched, stopping, terminal-awaiting-acknowledgement, and
/// unreconciled orphan work are active for Apply-admission purposes.
struct ResourceSlotAdmissionSnapshotDTO final {
  std::uint64_t operation_resource_generation{};
  /// Installed runtime identity, including candidate facts staged under an
  /// Apply fence. Apply/Disable reconciliation consumes this inventory.
  std::vector<DomainActiveControlConfigurationDTO> active_control_configurations;
  /// Only configurations with an authoritative Kernel route publication.
  /// CCLC route-fence admission consumes this inventory so an unready
  /// Apply-staged candidate cannot masquerade as a published predecessor.
  std::vector<DomainActiveControlConfigurationDTO> route_published_active_control_configurations;
  std::uint64_t affected_queued_operation_count{};
  std::uint64_t affected_active_operation_count{};
};

/// Fixed-inventory Kernel reservation table.
///
/// Disjoint operation/participant owners may hold slots concurrently;
/// overlapping scopes serialize at the exact resource slots. Every mutation
/// is atomic over the supplied resource set and no method performs I/O or
/// waits. A configuration fence is not a second slot state: the injected
/// admission gate serializes it with the real reservation while retaining the
/// durable fence identity separately.
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

  /// Reserve every participant scope atomically. Every row and resource is
  /// prevalidated before mutation, and success advances the table generation
  /// exactly once regardless of participant count.
  [[nodiscard]] Result<void> reserve_batch(
      const std::vector<ResourceSlotReservationOwnerDTO>& owners);

  [[nodiscard]] Result<void> transition(const ResourceSlotOwnerDTO& owner,
                                        const std::vector<ResourceId>& resources,
                                        ResourceSlotState next_state);

  /// Atomically transition every exact participant scope to one common legal
  /// state after a coordinated barrier. Every row is prevalidated and success
  /// advances the shared generation once.
  [[nodiscard]] Result<void> transition_batch(
      const std::vector<ResourceSlotReservationOwnerDTO>& owners, ResourceSlotState next_state);

  /// Transfer complete Dispatched scopes atomically, without releasing or
  /// reserving them. The exact coherent snapshot fences table generation,
  /// old owner/state and per-resource Stop generation. All targets name one
  /// valid operation/revision absent from the table, with distinct participants; no
  /// physical scope/configuration changes. State remains Dispatched and Stop
  /// generations remain unchanged; success advances the table generation once.
  ///
  /// The caller proves physical handover, same-Run/priority permission, durable
  /// identity freshness and admission/configuration fencing before this CAS.
  /// This table grants no dispatch or supersession authority and keeps no history.
  [[nodiscard]] Result<std::uint64_t> transfer_dispatched_owners(
      const ResourceSlotTableSnapshotDTO& expected,
      const std::vector<ResourceSlotOwnerTransferDTO>& transfers);

  /// Same exact batch CAS for a qualified moving handover or a settled cold
  /// handover. Settled transfers require Terminal and publish Reserved; the
  /// caller proves actual settlement/no-effect and local cleanup. Stop
  /// generations and physical binding stay exact in both modes.
  [[nodiscard]] Result<std::uint64_t> transfer_owners(
      const ResourceSlotTableSnapshotDTO& expected,
      const std::vector<ResourceSlotOwnerTransferDTO>& transfers,
      ResourceSlotOwnerTransferMode mode);

  /// Advance the owning operation revision after an accepted operation-scoped
  /// control without changing reservation state or resolved runtime facts.
  [[nodiscard]] Result<ResourceSlotOwnerDTO> revise_owner(
      const ResourceSlotOwnerDTO& owner, const std::vector<ResourceId>& resources,
      OperationRevision next_operation_revision);

  /// Atomically advance every participant owner of one coordinated operation
  /// to the same next revision without changing slot state.
  [[nodiscard]] Result<std::vector<ResourceSlotOwnerDTO>> revise_owner_batch(
      const std::vector<ResourceSlotReservationOwnerDTO>& owners,
      OperationRevision next_operation_revision);

  /// Atomically advance slot state and owning operation revision. This is the
  /// acceptance primitive for controls such as scoped Stop whose state and
  /// CAS fence must never be observably split.
  [[nodiscard]] Result<ResourceSlotOwnerDTO> transition_and_revise_owner(
      const ResourceSlotOwnerDTO& owner, const std::vector<ResourceId>& resources,
      ResourceSlotState next_state, OperationRevision next_operation_revision);

  /// Atomically transition and revise every participant owner of one
  /// coordinated operation. No participant observes a different operation
  /// revision or slot phase after acceptance.
  [[nodiscard]] Result<std::vector<ResourceSlotOwnerDTO>> transition_and_revise_owner_batch(
      const std::vector<ResourceSlotReservationOwnerDTO>& owners, ResourceSlotState next_state,
      OperationRevision next_operation_revision);

  /// Atomically fence a canonical set of distinct operation-participant owners
  /// plus any retained orphan/effect-uncertain resources. Every precondition is
  /// checked before any slot changes; success advances the table generation
  /// exactly once regardless of owner count.
  [[nodiscard]] Result<ResourceSlotStopFenceResultDTO> stop_fence_batch(
      const ResourceSlotStopFenceRequestDTO& request);

  /// Clear an exact pre-dispatch reservation or acknowledged terminal scope.
  [[nodiscard]] Result<void> release(const ResourceSlotOwnerDTO& owner,
                                     const std::vector<ResourceId>& resources);

  /// Atomically release a complete pre-dispatch or acknowledged-terminal
  /// coordinated scope. No partial release is observable.
  [[nodiscard]] Result<void> release_batch(
      const std::vector<ResourceSlotReservationOwnerDTO>& owners);

  /// Mark free resources conservatively unavailable after Kernel replacement.
  /// No prior operation owner is recreated and this performs no controller or
  /// transport effect. Later recovery must prove fresh quiescence before an
  /// explicit reconciliation surface may release the marks.
  [[nodiscard]] Result<void> mark_unreconciled_external_or_orphan_goal(
      const std::vector<ResourceId>& resources);

  /// Atomically release only the exact canonical orphan-resource subset named
  /// by a fresh owner-issued reconciliation snapshot. A stale table generation
  /// or any non-orphan row rejects before mutation.
  [[nodiscard]] Result<std::uint64_t> reconcile_unreconciled_external_or_orphan_goal(
      std::uint64_t expected_operation_resource_generation,
      const std::vector<ResourceId>& resources);

  /// Replace the selected runtime inventory while preserving every unchanged
  /// occupied slot. An empty candidate is the valid no-commandable-component
  /// product. Removed resources must already be free, and any retained
  /// operation must still name the exact active control configuration selected
  /// by the candidate runtime. The caller is responsible for holding the
  /// configuration fence that covers every changed resource.
  [[nodiscard]] Result<void> reconfigure_runtime_inventory(
      std::vector<ResourceId> resources,
      std::vector<DomainActiveControlConfigurationDTO> active_control_configurations,
      std::vector<DomainActiveControlConfigurationDTO>
          route_published_active_control_configurations);

  /// Replace only the authoritative route-publication identities. The caller
  /// uses the shared admission mutex to join this mutation atomically with
  /// its durable runtime-route readback update.
  [[nodiscard]] Result<void> replace_route_publication_inventory(
      std::vector<DomainActiveControlConfigurationDTO>
          route_published_active_control_configurations);

  [[nodiscard]] std::vector<ResourceSlotSnapshotDTO> snapshot() const;
  [[nodiscard]] ResourceSlotTableSnapshotDTO coherent_snapshot() const;
  [[nodiscard]] ResourceSlotAdmissionSnapshotDTO admission_snapshot(
      const std::vector<ResourceId>& affected_resources) const;
  /// Canonical unique operation owners currently retained by at least one
  /// slot. Orphan-unreconciled slots have no operation owner and are omitted.
  [[nodiscard]] std::vector<OperationId> active_operation_ids() const;
  [[nodiscard]] bool shares_admission_mutex(
      const std::shared_ptr<std::recursive_mutex>& admission_mutex) const noexcept;

 private:
  [[nodiscard]] Result<void> validate_resource_set_locked(const std::vector<ResourceId>& resources,
                                                          std::vector<std::size_t>& indices) const;
  [[nodiscard]] Result<void> require_generation_available_locked() const;
  [[nodiscard]] Result<std::vector<ResourceSlotOwnerDTO>> revise_owner_batch_locked(
      const std::vector<ResourceSlotReservationOwnerDTO>& owners,
      std::optional<ResourceSlotState> next_state, OperationRevision next_operation_revision);
  void advance_generation_locked() noexcept;

  // The admission mutex orders new operation/configuration admission with
  // external durable fences. Slot bytes have a distinct lock so recognized
  // Stop can inspect and fence the bounded table without waiting for an
  // unrelated durable admission write. Kernel callers establish the required
  // state/admission ordering before entering these data-only operations.
  std::shared_ptr<std::recursive_mutex> admission_mutex_;
  mutable std::recursive_mutex data_mutex_;
  std::vector<ResourceSlotSnapshotDTO> slots_;
  std::vector<DomainActiveControlConfigurationDTO> active_control_configurations_;
  std::vector<DomainActiveControlConfigurationDTO> route_published_active_control_configurations_;
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
  [[nodiscard]] virtual Result<void> reserve_batch(
      ResourceSlotTable& table, const std::vector<ResourceSlotReservationOwnerDTO>& owners) = 0;
  /// Read-only exact overlap with process-local durable configuration fences.
  /// This is private recovery evidence, not a caller-visible lease or an
  /// alternative resource-state owner.
  [[nodiscard]] virtual std::vector<ResourceId> current_fenced_resources(
      const std::vector<ResourceId>& resources) const = 0;
  /// Whether the one exact held fence covers every listed resource. Coverage
  /// by any other fence does not count. Gates without fence identity refuse.
  [[nodiscard]] virtual bool exact_fence_covers(
      const ConfigurationFenceId& /*fence_id*/,
      const std::vector<ResourceId>& /*resources*/) const {
    return false;
  }

  /// Any held configuration fence, including an empty-scope Apply. Callers
  /// retain shared_admission_mutex() through the protected installation step.
  /// Gates without this proof fail closed for that installation only.
  [[nodiscard]] virtual bool has_held_configuration_fences() const { return true; }

  /// Exact held scope check, including empty-scope Applies that only re-seal
  /// product bindings. Coverage by a different or wider fence is insufficient.
  [[nodiscard]] virtual bool exact_fence_is_held(
      const ConfigurationFenceId& /*fence_id*/, const ConfigurationFenceScopeDTO& /*scope*/) const {
    return false;
  }
};

}  // namespace manyforge::core
