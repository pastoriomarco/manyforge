#pragma once

#include <cstddef>
#include <vector>

#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/types/operation_intent.hpp>

namespace manyforge::core {

/// Fixed active-runtime registry for the selected Program Bindings.
///
/// Component/target/capability resolution is exact and order-independent.
/// The registry contains no backend discovery, name-prefix fallback, or first
/// robot/group behavior.
class RuntimeParticipantRegistry final {
 public:
  explicit RuntimeParticipantRegistry(
      std::vector<RuntimeParticipantBinding> bindings);

  [[nodiscard]] Result<RuntimeParticipantBinding> resolve(
      const OperationIntentDTO& operation,
      std::size_t participant_index) const;

  /// Validate one externally resolved sealed binding against the exact
  /// operation address and payload. This supports a bounded product-backed
  /// resolver without weakening the same checks used by the fixed registry.
  [[nodiscard]] static Result<RuntimeParticipantBinding> validate_resolved(
      const OperationIntentDTO& operation, std::size_t participant_index,
      const RuntimeParticipantBinding& binding);

  [[nodiscard]] const std::vector<ResourceId>& resource_inventory()
      const noexcept {
    return resource_inventory_;
  }

 private:
  std::vector<RuntimeParticipantBinding> bindings_;
  std::vector<ResourceId> resource_inventory_;
};

}  // namespace manyforge::core
