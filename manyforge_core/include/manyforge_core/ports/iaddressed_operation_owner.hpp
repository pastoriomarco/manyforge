#pragma once

#include <manyforge_core/types/application_identity.hpp>
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
      const OperationNumber& operation_id) const = 0;
  [[nodiscard]] virtual Result<void> reconcile_subordinate_state() = 0;
  [[nodiscard]] virtual ApplicationStartId application_start_id() const noexcept = 0;
};

}  // namespace manyforge::core
