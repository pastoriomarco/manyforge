#pragma once

#include <manyforge_core/ports/iexecution_backend.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/execution_status.hpp>
#include <manyforge_core/types/motion_interrupt.hpp>

#include <cstdint>
#include <map>
#include <string>

namespace manyforge::core {

inline constexpr std::int64_t kExecutionPollNs = 10'000'000;  // 10 ms
inline constexpr int kExecutionDrainMaxPolls = 100;  // bounded 1 s drain
inline constexpr double kExecutionDrainTimeoutS =
    static_cast<double>(kExecutionPollNs * kExecutionDrainMaxPolls) / 1e9;

const char* stop_mode_string(MotionInterruptStopMode mode);
const char* stop_command_source_string(MotionInterruptStopCommandSource source);

bool drain_execution_handle_bounded(ExecutionHandle& handle);
ErrorDTO operator_intervention_error(ErrorCode code,
                                     std::string message,
                                     std::string phase,
                                     ExecutionState previous_state,
                                     std::uint64_t execution_id,
                                     const std::string& move_id);

}  // namespace manyforge::core
