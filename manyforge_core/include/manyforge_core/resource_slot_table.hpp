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
#include <span>
#include <string_view>
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
  OperationNumber operation_id;
  OperationRevision operation_revision;
  OperationParticipantId participant_id;
  ParticipantBindingIdentityDTO binding_identity;
  ResourceSlotControllerAuthorityDTO controller_authority;

  ResourceSlotOwnerDTO() = default;
  ResourceSlotOwnerDTO(OperationNumber operation, OperationRevision revision,
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
  /// The latest Kernel Stop event that fenced this resource (0 = none). Event
  /// ids rise per application start, so it only rises, across terminal
  /// acknowledgement and reuse.
  StopEventId stop_event_id{0U};

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

/// One participant row of a Stop fence (design §3.1 E1). It names the
/// participant's own slot owner and resource list (non-empty, sorted, unique;
/// disjoint across the batch); nothing is copied. Rows are ordered by
/// (operation_id, participant_id). `fence`: move the slots to kStopping and
/// stamp the event; false: raise only the owner's operation revision (an
/// operation's participant that this Stop does not start).
struct ResourceSlotStopFenceRow final {
  const ResourceSlotOwnerDTO* expected_owner{};
  std::span<const ResourceId> resources{};
  OperationRevision next_operation_revision{0U};
  bool fence{};
};

/// Kernel-owned scratch of `stop_fence_batch`, allocated once at Kernel
/// construction (`stop_fence_scratch_`) for `owner_capacity` (`N_max`) rows
/// and `kMaxKernelResourceSlots` slots, so a Stop fence allocates nothing. The
/// Kernel fills the rows (`clear`, `add_owner`) under its state lock; only the
/// table writes the slot fields, under its data lock.
struct ResourceSlotStopFenceScratch final {
  explicit ResourceSlotStopFenceScratch(const std::size_t owner_capacity) {
    rows.reserve(owner_capacity);
    slot_indices.reserve(kMaxKernelResourceSlots);
    claimed.assign(kMaxKernelResourceSlots, 0U);
  }
  void clear() noexcept { rows.clear(); }
  /// One participant row: its own slot owner and resources (not copied; they
  /// must stay unchanged until the fence returns). `revision_only`: raise
  /// only the owner's operation revision (a participant of the operation that
  /// this Stop does not start). Rows within the reserved capacity never
  /// allocate.
  void add_owner(const ResourceSlotOwnerDTO& owner, const std::span<const ResourceId> resources,
                 const OperationRevision next_operation_revision,
                 const bool revision_only) noexcept {
    if (rows.size() == rows.capacity()) return;
    rows.push_back(ResourceSlotStopFenceRow{&owner, resources, next_operation_revision,
                                            !revision_only});
  }
  std::vector<ResourceSlotStopFenceRow> rows{};
  /// Slot index of every row resource, rows in order.
  std::vector<std::size_t> slot_indices{};
  /// Per slot index: claimed by a row of the current batch (overlap check);
  /// all zero between calls.
  std::vector<std::uint8_t> claimed{};
  /// The table generation after the last successful fence.
  std::uint64_t operation_resource_generation{};
};

/// One atomic read of the operation/resource truth owned by a slot table.
/// Counts are operation counts, not resource counts. Reserved operations are
/// queued; dispatched, stopping and terminal-awaiting-acknowledgement work is
/// active for Apply-admission purposes.
struct ResourceSlotAdmissionSnapshotDTO final {
  std::uint64_t operation_resource_generation{};
  /// Installed runtime identity, including candidate facts staged under an
  /// Apply fence. Apply/Disable reconciliation consumes this inventory.
  std::vector<DomainActiveControlConfigurationDTO> active_control_configurations;
  /// Only configurations with an authoritative Kernel route publication.
  /// Kernel route-fence admission consumes this inventory so an unready
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

  /// E1: atomically fence the participant rows of one Stop event. Every
  /// precondition is checked before any slot changes: each row's owner matches
  /// its slots; a fenced row's slots are reserved, dispatched or terminal (a
  /// terminal observation does not prove that an in-flight provider call
  /// returned, so its exact Stop fence stays admissible); each row's revision
  /// advances; scopes are disjoint. Success moves fenced slots to kStopping,
  /// stamps `stop_event_id`, writes every row's next operation revision into
  /// its slots' owner and advances the table generation once. Called under the
  /// Kernel's state lock: takes only the data lock, allocates nothing, never
  /// throws. The rows are `scratch.rows`. Returns nothing on success, else
  /// the refusal reason code (the Kernel records it as `fence_degraded`).
  [[nodiscard]] std::optional<std::string_view> stop_fence_batch(
      StopEventId stop_event_id, ResourceSlotStopFenceScratch& scratch) noexcept;

  /// Clear an exact pre-dispatch reservation or acknowledged terminal scope.
  [[nodiscard]] Result<void> release(const ResourceSlotOwnerDTO& owner,
                                     const std::vector<ResourceId>& resources);

  /// Atomically release a complete pre-dispatch or acknowledged-terminal
  /// coordinated scope. No partial release is observable.
  [[nodiscard]] Result<void> release_batch(
      const std::vector<ResourceSlotReservationOwnerDTO>& owners);

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
  /// Whether any of `resources` is still owned by `owner` (same operation
  /// and participant). Takes only the data lock; allocates nothing. The Stop
  /// core calls it under the Kernel lock.
  [[nodiscard]] bool owner_holds_any(const ResourceSlotOwnerDTO& owner,
                                     std::span<const ResourceId> resources) const noexcept;
  [[nodiscard]] ResourceSlotTableSnapshotDTO coherent_snapshot() const;
  [[nodiscard]] ResourceSlotAdmissionSnapshotDTO admission_snapshot(
      const std::vector<ResourceId>& affected_resources) const;
  /// Canonical unique operation owners currently retained by at least one
  /// slot.
  [[nodiscard]] std::vector<OperationNumber> active_operation_ids() const;
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
