#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>

#include <manyforge_core/types/kernel_event.hpp>

namespace manyforge::core {

KernelEvent make_kernel_event(
    KernelEventType type,
    std::uint64_t execution_id,
    std::int64_t timestamp_ns,
    std::map<std::string, std::string> details = {});

KernelEvent make_state_transition_event(
    std::uint64_t execution_id,
    std::int64_t timestamp_ns,
    ExecutionState from_state,
    ExecutionState to_state,
    std::map<std::string, std::string> details = {});

KernelEvent make_error_raised_event(
    std::uint64_t execution_id,
    std::int64_t timestamp_ns,
    ErrorDTO error,
    std::map<std::string, std::string> details = {});

KernelEvent make_error_raised_event(
    std::uint64_t execution_id,
    std::int64_t timestamp_ns,
    std::optional<ErrorDTO> error,
    std::map<std::string, std::string> details = {});

}  // namespace manyforge::core
