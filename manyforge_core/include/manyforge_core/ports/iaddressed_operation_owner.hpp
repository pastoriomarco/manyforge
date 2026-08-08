#pragma once

#include <manyforge_core/types/operation_runtime.hpp>

namespace manyforge::core {

/// Focused NRT port consumed by higher-level addressed sequences. The concrete
/// ResourceAddressedExecutionKernel remains the sole operation/resource owner;
/// adapters may forward this exact contract across a bounded private channel.
class IAddressedOperationOwner {
 public:
  virtual ~IAddressedOperationOwner() = default;

  [[nodiscard]] virtual Result<OperationAcceptedDTO> execute(
      const OperationIntentDTO& intent) = 0;
  [[nodiscard]] virtual Result<OperationControlAcceptedDTO>
  acknowledge_completed(const OperationControlRequestDTO& request) = 0;
  [[nodiscard]] virtual Result<OperationStateDTO> get_operation_state(
      const OperationId& operation_id) const = 0;
  [[nodiscard]] virtual Result<void> reconcile_subordinate_state() = 0;
  [[nodiscard]] virtual const RequestOwnerEpoch& request_owner_epoch()
      const noexcept = 0;
};

}  // namespace manyforge::core
