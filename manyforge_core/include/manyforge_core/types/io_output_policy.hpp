#pragma once

#include <manyforge_core/time/iclock.hpp>
#include <variant>

namespace manyforge::core {

struct IoOutputImmediateDTO final {
  friend bool operator==(const IoOutputImmediateDTO&, const IoOutputImmediateDTO&) = default;
};

struct IoOutputSourceAckBudgetDTO final {
  Duration timeout_ns;
  friend bool operator==(const IoOutputSourceAckBudgetDTO&,
                         const IoOutputSourceAckBudgetDTO&) = default;
};

/// A publish-only mechanism completes on its correlated submission receipt.
/// This is neither source acknowledgement nor immediate synthetic acceptance.
struct IoOutputSubmittedDTO final {
  friend bool operator==(const IoOutputSubmittedDTO&, const IoOutputSubmittedDTO&) = default;
};

/// Resolved from the sealed output mechanism/device, never from a command
/// caller or per-signal completion criterion. Immediate has no invented wait.
struct IoOutputCompletionPolicyDTO final {
  std::variant<IoOutputImmediateDTO, IoOutputSourceAckBudgetDTO, IoOutputSubmittedDTO> value;

  explicit IoOutputCompletionPolicyDTO(IoOutputImmediateDTO immediate) : value(immediate) {}
  explicit IoOutputCompletionPolicyDTO(IoOutputSourceAckBudgetDTO budget) : value(budget) {}
  explicit IoOutputCompletionPolicyDTO(IoOutputSubmittedDTO submitted) : value(submitted) {}

  [[nodiscard]] const IoOutputSourceAckBudgetDTO* source_ack_budget() const noexcept {
    return std::get_if<IoOutputSourceAckBudgetDTO>(&value);
  }
  [[nodiscard]] bool submitted_only() const noexcept {
    return std::holds_alternative<IoOutputSubmittedDTO>(value);
  }
  [[nodiscard]] bool locally_valid() const noexcept {
    if (value.valueless_by_exception()) return false;
    const auto* budget = source_ack_budget();
    return budget == nullptr || budget->timeout_ns > 0;
  }
  friend bool operator==(const IoOutputCompletionPolicyDTO&,
                         const IoOutputCompletionPolicyDTO&) = default;
};

}  // namespace manyforge::core
