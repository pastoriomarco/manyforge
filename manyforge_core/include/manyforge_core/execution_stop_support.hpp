#pragma once

#include <manyforge_core/ports/iexecution_backend.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/time/ischeduler.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/execution_result.hpp>
#include <manyforge_core/types/execution_status.hpp>
#include <manyforge_core/types/motion_interrupt.hpp>
#include <manyforge_core/types/robot_state.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>

namespace manyforge::core {

inline constexpr std::int64_t kExecutionPollNs = 10'000'000;  // 10 ms
inline constexpr int kExecutionDrainMaxPolls = 100;  // bounded 1 s drain
inline constexpr double kExecutionDrainTimeoutS =
    static_cast<double>(kExecutionPollNs * kExecutionDrainMaxPolls) / 1e9;
inline constexpr std::int64_t kFaultPreemptPendingSlackNs = 250'000'000;

// Per stop attempt; shared only by that attempt's watchdog/fallback paths.
struct HardStopFloorAttempt {
  std::mutex mutex;
  bool attempted = false;
  StopResultDTO result;
};

struct MeasuredRestWaitResult {
  std::optional<double> max_abs_velocity_rad_s;
  bool settle_signal_available = false;
  bool at_rest = true;
  bool timed_out = false;
  bool aborted = false;
};

struct StopFallbackQualificationRow {
  MotionInterruptStopMode requested;
  MotionInterruptStopMode executed;
  MotionInterruptNodeCategory node_category;
  std::array<MotionInterruptStopCommandSource, 5> allowed_command_sources;
  std::size_t allowed_command_source_count;
  const char* qualification_id;
  const char* rationale;
  const char* telemetry;
  const char* evidence_status;
};

const char* stop_mode_string(MotionInterruptStopMode mode);
const char* stop_command_source_string(MotionInterruptStopCommandSource source);

StopResultDTO request_hard_stop_floor_once(
    IExecutionBackend& backend,
    const std::shared_ptr<HardStopFloorAttempt>& attempt);
std::optional<StopResultDTO> hard_stop_floor_result_if_attempted(
    const std::shared_ptr<HardStopFloorAttempt>& attempt);

void add_hard_stop_floor_details(std::map<std::string, std::string>& details,
                                 IExecutionBackend& backend);
std::map<std::string, std::string> controlled_stop_success_event_details(
    const MotionInterruptStopProfileDTO& profile,
    const ControlledStopResultDTO& result,
    IExecutionBackend& backend,
    const std::string& phase);

std::int64_t effective_controlled_stop_deadline_ns(
    const MotionInterruptStopProfileDTO& profile,
    Duration stop_completion_tolerance_ns);
std::chrono::steady_clock::duration effective_controlled_stop_deadline_duration(
    const MotionInterruptStopProfileDTO& profile,
    Duration stop_completion_tolerance_ns);

std::optional<double> measured_max_abs_velocity_from_snapshot(
    const ExecutionSnapshot& snapshot);
bool all_joint_positions_finite(const ExecutionSnapshot& snapshot);
bool all_joint_velocities_finite_when_present(
    const ExecutionSnapshot& snapshot);
std::optional<double> parse_positive_double_detail(
    const std::map<std::string, std::string>& details,
    const std::string& key);
bool is_classified_stop_intervention(
    const std::map<std::string, std::string>& details);
bool is_command_authoritative_stop_snapshot(
    const ExecutionSnapshot& snapshot);
MeasuredRestWaitResult wait_for_measured_rest_after_confirmed_stop(
    IExecutionBackend& backend,
    double settle_velocity_threshold_rad_s,
    TimePoint deadline,
    IClock& clock,
    IScheduler& scheduler,
    std::stop_token stop_token,
    std::function<bool()> abort_requested = {});
bool operator_controlled_stop_allowed(ExecutionState state);

const StopFallbackQualificationRow* qualified_stop_fallback_pair(
    MotionInterruptStopMode requested,
    MotionInterruptStopMode executed,
    MotionInterruptNodeCategory node_category);
bool fallback_row_allows_command_source(
    const StopFallbackQualificationRow& row,
    MotionInterruptStopCommandSource source);
bool robot_state_size_ordering_ok(const RobotStateDTO& state);
std::optional<ExecutionStopSnapshotDTO> capture_stop_snapshot(
    const ExecutionSnapshot& post_stop_backend_snapshot,
    const std::optional<RobotStateDTO>& observed_state_provider_state,
    bool backend_stop_confirmed,
    MotionInterruptNodeCategory node_category,
    std::int64_t captured_at_ns,
    double settle_velocity_threshold_rad_s,
    std::string& failure_reason);

bool drain_execution_handle_bounded(ExecutionHandle& handle);
ErrorDTO operator_intervention_error(ErrorCode code,
                                     std::string message,
                                     std::string phase,
                                     ExecutionState previous_state,
                                     std::uint64_t execution_id,
                                     const std::string& move_id);
ErrorDTO ensure_operator_intervention_details(ErrorDTO error,
                                              std::string phase,
                                              ExecutionState previous_state,
                                              std::uint64_t execution_id,
                                              const std::string& move_id);

}  // namespace manyforge::core
