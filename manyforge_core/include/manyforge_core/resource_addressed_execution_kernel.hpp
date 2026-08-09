#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <manyforge_core/ports/iaddressed_motion_preview.hpp>
#include <manyforge_core/ports/iaddressed_operation_owner.hpp>
#include <manyforge_core/ports/ioperation_journal.hpp>
#include <manyforge_core/ports/iresolved_participant_execution_engine.hpp>
#include <manyforge_core/resource_slot_table.hpp>
#include <manyforge_core/runtime_participant_registry.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace manyforge::core {

inline constexpr std::size_t kMaxI6OperationReplayEntries = 4096U;

struct RuntimeParticipantExecutionRegistration final {
  RuntimeParticipantBinding binding;

  /// Non-owning. The engine must remain alive and quiesce callbacks before
  /// this addressed owner is destroyed.
  IResolvedParticipantExecutionEngine* engine{};
};

using RuntimeParticipantAdmissionResolver =
    std::function<Result<RuntimeParticipantExecutionRegistration>(const OperationIntentDTO&,
                                                                  std::size_t)>;

struct ResourceAddressedExecutionKernelConfig final {
  ResourceAddressedExecutionKernelConfig() = default;
  ResourceAddressedExecutionKernelConfig(RequestOwnerEpoch owner_epoch, std::size_t replay_capacity,
                                         RuntimeParticipantAdmissionResolver resolver = {},
                                         IResourceOperationAdmissionGate* admission_gate = nullptr)
      : request_owner_epoch(std::move(owner_epoch)),
        request_replay_capacity(replay_capacity),
        participant_resolver(std::move(resolver)),
        operation_admission_gate(admission_gate) {}

  RequestOwnerEpoch request_owner_epoch;
  std::size_t request_replay_capacity{1024U};
  /// Optional exact product-backed lookup. The Kernel revalidates the sealed
  /// binding against the request and accepts only a lifetime-pinned engine
  /// already present in registrations.
  RuntimeParticipantAdmissionResolver participant_resolver;
  /// Optional process-local configuration-fence interlock. It must outlive
  /// this Kernel. The interlock serializes the real slot-table reservation;
  /// it owns no duplicate operation counters or resource state.
  IResourceOperationAdmissionGate* operation_admission_gate{};
};

/// Private no-effect result used to project a capability-command preview at a
/// trusted transport boundary. The public receipt is minted by that boundary;
/// this value carries no reusable admission or reservation.
struct CapabilityCommandPreviewResultDTO final {
  RuntimeParticipantBindingDTO binding;
  GripperCommandRequestDTO command_request;
};

/// I6 resource-addressed Kernel owner.
///
/// The current restoration gate deliberately admits one operation with one
/// participant. Existing ExecutionKernel instances remain private subordinate
/// planning/execution mechanisms. I8 later expands operation coordination and
/// concurrency without changing the public component/target/resource identity.
class ResourceAddressedExecutionKernel final : public IAddressedOperationOwner,
                                               public IAddressedMotionPreviewOwner {
 public:
  ResourceAddressedExecutionKernel(
      std::vector<RuntimeParticipantExecutionRegistration> registrations, IClock& clock,
      IOperationJournal& journal, ResourceAddressedExecutionKernelConfig config);
  ~ResourceAddressedExecutionKernel();

  ResourceAddressedExecutionKernel(const ResourceAddressedExecutionKernel&) = delete;
  ResourceAddressedExecutionKernel& operator=(const ResourceAddressedExecutionKernel&) = delete;

  /// Resolve, reserve, and submit one portable operation. Exact replay returns
  /// the original owner-minted identities and performs no subordinate call.
  [[nodiscard]] Result<OperationAcceptedDTO> execute(const OperationIntentDTO& intent) override;

  [[nodiscard]] Result<std::unique_ptr<IAddressedMotionPreviewSession>>
  begin_motion_preview_session(const OperationIntentDTO& primary_motion) override;

  /// Resolve and validate one capability command without reserving,
  /// journaling, changing observed state, or dispatching a command.
  [[nodiscard]] Result<CapabilityCommandPreviewResultDTO> preview_capability_command(
      const OperationIntentDTO& intent);

  [[nodiscard]] Result<OperationControlAcceptedDTO> pause(
      const OperationControlRequestDTO& request);
  [[nodiscard]] Result<OperationControlAcceptedDTO> resume(
      const OperationControlRequestDTO& request);
  [[nodiscard]] Result<OperationControlAcceptedDTO> reset(
      const OperationControlRequestDTO& request);
  [[nodiscard]] Result<OperationControlAcceptedDTO> acknowledge_completed(
      const OperationControlRequestDTO& request) override;

  [[nodiscard]] Result<StopAcceptedDTO> stop(const StopIntentDTO& intent);
  [[nodiscard]] Result<RuntimeScaleAcceptedDTO> set_runtime_scale(
      const RuntimeScaleRequestDTO& request);
  [[nodiscard]] Result<RuntimeScaleStatusDTO> get_runtime_scale(
      const OperationId& operation_id) const;

  /// Read-only owner projection. Subordinate polling and state advancement are
  /// performed only by reconcile_subordinate_state(), never by this method.
  [[nodiscard]] Result<OperationStateDTO> get_operation_state(
      const OperationId& operation_id) const override;

  /// NRT owner-scheduler reconciliation point. This closes event-loss windows
  /// and advances capability-command completion without making public reads
  /// effectful.
  [[nodiscard]] Result<void> reconcile_subordinate_state() override;

  [[nodiscard]] std::vector<ResourceSlotSnapshotDTO> resource_snapshot() const;

  /// Atomically select a verified candidate participant set while preserving
  /// the stable addressed owner, operation journal, replay ledgers, and any
  /// unaffected in-flight operation. The caller must hold the exact durable
  /// configuration fence for every changed resource.
  [[nodiscard]] Result<void> reconfigure_runtime_participants(
      std::vector<RuntimeParticipantExecutionRegistration> registrations,
      RuntimeParticipantAdmissionResolver participant_resolver = {});

  /// A removed subordinate engine stays lifetime-pinned while an operation
  /// still references it. Once this returns false, release_retired_engine()
  /// removes the remaining event subscription before the caller destroys the
  /// engine.
  [[nodiscard]] bool participant_engine_in_use(
      const IResolvedParticipantExecutionEngine* engine) const;
  [[nodiscard]] Result<void> release_retired_engine(IResolvedParticipantExecutionEngine* engine);

  [[nodiscard]] std::shared_ptr<const ResourceSlotTable> resource_admission_table() const noexcept {
    return resource_slots_;
  }
  [[nodiscard]] const RequestOwnerEpoch& request_owner_epoch() const noexcept override {
    return config_.request_owner_epoch;
  }

 private:
  struct ReplayEntry;
  struct ControlReplayEntry;
  struct StopReplayEntry;
  struct RuntimeScaleReplayEntry;
  struct ActiveOperation;
  struct EngineSubscription;
  struct PreviewSession;

  [[nodiscard]] IResolvedParticipantExecutionEngine* engine_for(
      const RuntimeParticipantBinding& binding) const noexcept;
  [[nodiscard]] Result<RuntimeParticipantExecutionRegistration> resolve_registration(
      const OperationIntentDTO& operation, std::size_t participant_index) const;
  void on_engine_event(IResolvedParticipantExecutionEngine* engine, const KernelEvent& event);
  [[nodiscard]] Result<void> apply_subordinate_state(IResolvedParticipantExecutionEngine* engine,
                                                     std::uint64_t subordinate_id,
                                                     OperationStateKind state, double progress,
                                                     std::optional<ErrorDTO> terminal_error);
  [[nodiscard]] Result<OperationControlAcceptedDTO> apply_control(
      const OperationControlRequestDTO& request, OperationControlKind control);
  [[nodiscard]] Result<void> apply_runtime_scale_status(
      IResolvedParticipantExecutionEngine* engine,
      const ResolvedRuntimeScaleStatusDTO& resolved_status);

  RuntimeParticipantRegistry registry_;
  std::shared_ptr<ResourceSlotTable> resource_slots_;
  std::vector<RuntimeParticipantExecutionRegistration> registrations_;
  IClock& clock_;
  IOperationJournal& journal_;
  ResourceAddressedExecutionKernelConfig config_;

  // Serializes public Execute admission while permitting synchronous engine
  // callbacks to take state_mutex_. The subordinate execute call is required
  // to be prompt/non-blocking by its port contract.
  mutable std::mutex submission_mutex_;
  mutable std::mutex control_mutex_;
  mutable std::mutex state_mutex_;
  std::vector<ReplayEntry> replay_ledger_;
  std::vector<ControlReplayEntry> control_replay_ledger_;
  std::vector<StopReplayEntry> stop_replay_ledger_;
  std::vector<RuntimeScaleReplayEntry> runtime_scale_replay_ledger_;
  std::vector<OperationStateDTO> historical_operations_;
  std::unique_ptr<ActiveOperation> active_operation_;
  std::vector<EngineSubscription> engine_subscriptions_;
  std::uint64_t next_operation_id_{1U};
  std::uint64_t next_stop_id_{1U};
};

}  // namespace manyforge::core
