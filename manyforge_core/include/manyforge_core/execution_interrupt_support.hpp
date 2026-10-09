#pragma once

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/interrupt.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace manyforge::core {

inline constexpr const char* kRuntimeCollisionFaultSourceId =
    "manyforge.kernel.runtime_collision_fault";

bool validation_cancelled_by_interrupt(const ErrorDTO& error);
bool authoritative_collision_replan_candidate(const ErrorDTO& error);
ErrorDTO authoritative_collision_validation_config_error(
    std::string reason,
    std::map<std::string, std::string> details = {},
    std::string phase = "pre_execution_validation");
const char* interrupt_preemption_result_name(
    InterruptPreemptionResult result) noexcept;

}  // namespace manyforge::core
