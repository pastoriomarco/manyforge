#pragma once

#include <cstddef>
#include <cstdint>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/operation_runtime.hpp>
#include <vector>

namespace manyforge::core {

struct OperationJournalRecoveryDTO final {
  std::vector<OperationStateDTO> historical_operations;
  std::vector<ResourceId> unreconciled_resources;
};

/// Durable boundary owned by the resource-addressed Execution Kernel.
///
/// Every acceptance method commits the complete public intent, owner-produced
/// result, and resulting immutable state in one transaction. The Kernel calls
/// it before the corresponding subordinate effect. State observations are
/// committed before they become visible through owner projections.
class IOperationJournal {
 public:
  virtual ~IOperationJournal() = default;

  [[nodiscard]] virtual Result<OperationJournalRecoveryDTO> BeginOwnerEpoch(
      const RequestOwnerEpoch& owner_epoch, std::int64_t started_at_ns,
      std::size_t request_capacity, const std::vector<ResourceId>& resource_inventory) = 0;

  [[nodiscard]] virtual Result<std::uint64_t> RecordOperationAccepted(
      const RequestOwnerEpoch& owner_epoch, const OperationIntentDTO& intent,
      const OperationAcceptedDTO& accepted, const OperationStateDTO& state) = 0;

  [[nodiscard]] virtual Result<std::uint64_t> RecordControlAccepted(
      const RequestOwnerEpoch& owner_epoch, const OperationControlRequestDTO& request,
      const OperationControlAcceptedDTO& accepted, const OperationStateDTO& state) = 0;

  [[nodiscard]] virtual Result<std::uint64_t> RecordStopAccepted(
      const RequestOwnerEpoch& owner_epoch, const StopIntentDTO& intent,
      const StopAcceptedDTO& accepted, const OperationStateDTO& state) = 0;

  [[nodiscard]] virtual Result<std::uint64_t> RecordRuntimeScaleAccepted(
      const RequestOwnerEpoch& owner_epoch, const RuntimeScaleRequestDTO& request,
      const RuntimeScaleAcceptedDTO& accepted, const OperationStateDTO& state,
      const RuntimeScaleStatusDTO& scale_status) = 0;

  [[nodiscard]] virtual Result<std::uint64_t> RecordOperationState(
      const RequestOwnerEpoch& owner_epoch, const OperationStateDTO& state,
      const RuntimeScaleStatusDTO* scale_status, const char* transition_reason) = 0;
};

}  // namespace manyforge::core
