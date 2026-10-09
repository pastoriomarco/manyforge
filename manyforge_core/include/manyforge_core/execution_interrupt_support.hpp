#pragma once

#include <manyforge_core/interrupt/interrupt_manager.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/kernel_event.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace manyforge::core {

inline constexpr const char* kRuntimeCollisionFaultSourceId =
    "manyforge.kernel.runtime_collision_fault";
inline constexpr const char* kRuntimeCollisionFaultPolicyId =
    "manyforge.kernel.runtime_collision_fault.preempt_trajectory";

bool validation_cancelled_by_interrupt(const ErrorDTO& error);
bool authoritative_collision_replan_candidate(const ErrorDTO& error);
std::optional<KernelEventType> interrupt_event_type_from_name(
    const std::string& name);
InterruptIngestResult reject_interrupts_without_active_phase(
    const std::vector<InterruptSignalUpdate>& updates);
ErrorDTO authoritative_collision_validation_config_error(
    std::string reason,
    std::map<std::string, std::string> details = {},
    std::string phase = "pre_execution_validation");
const char* interrupt_preemption_result_name(
    InterruptPreemptionResult result) noexcept;
InterruptSourceDefinition runtime_collision_fault_source_definition();
InterruptPolicyDefinition runtime_collision_fault_policy_definition();

}  // namespace manyforge::core
