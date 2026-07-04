#include <manyforge_core/types/kernel_event_factories.hpp>

#include <utility>

namespace manyforge::core {

KernelEvent make_kernel_event(
    KernelEventType type,
    std::uint64_t execution_id,
    std::int64_t timestamp_ns,
    std::map<std::string, std::string> details) {
  return KernelEvent{type,
                     execution_id,
                     timestamp_ns,
                     ExecutionState::kIdle,
                     ExecutionState::kIdle,
                     std::nullopt,
                     std::move(details)};
}

KernelEvent make_state_transition_event(
    std::uint64_t execution_id,
    std::int64_t timestamp_ns,
    ExecutionState from_state,
    ExecutionState to_state,
    std::map<std::string, std::string> details) {
  return KernelEvent{KernelEventType::kStateTransition,
                     execution_id,
                     timestamp_ns,
                     from_state,
                     to_state,
                     std::nullopt,
                     std::move(details)};
}

KernelEvent make_error_raised_event(
    std::uint64_t execution_id,
    std::int64_t timestamp_ns,
    ErrorDTO error,
    std::map<std::string, std::string> details) {
  return make_error_raised_event(execution_id, timestamp_ns,
                                 std::optional<ErrorDTO>{std::move(error)},
                                 std::move(details));
}

KernelEvent make_error_raised_event(
    std::uint64_t execution_id,
    std::int64_t timestamp_ns,
    std::optional<ErrorDTO> error,
    std::map<std::string, std::string> details) {
  return KernelEvent{KernelEventType::kErrorRaised,
                     execution_id,
                     timestamp_ns,
                     ExecutionState::kIdle,
                     ExecutionState::kIdle,
                     std::move(error),
                     std::move(details)};
}

}  // namespace manyforge::core
