#include <manyforge_ros2_control_execution/ros2_control_execution_backend.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <future>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <lifecycle_msgs/msg/state.hpp>
#include <manyforge_core/motion_interrupt/soft_on_path_stop_planner.hpp>
#include <manyforge_core/execution_stop_support.hpp>
#include <manyforge_core/trajectory/trajectory_sampler.hpp>
#include <manyforge_core/types/error_code.hpp>

#include "scene_projection_worker.hpp"
#include "ros2_control_stop_qualification_probe.hpp"

namespace manyforge::ros2_control_execution {

void detail::SceneProjectionWorkerDeleter::operator()(
    detail::SceneProjectionWorker *worker) const noexcept {
  delete worker;
}

namespace {

constexpr double kRuntimeScaleSlewPerSecond = 4.0; // 0→1 in 250 ms.
constexpr auto kIdleSceneProjectionMinPeriod = std::chrono::milliseconds(100);
constexpr auto kSceneProjectionBarrierTimeout = std::chrono::seconds(1);

std::string new_component_epoch(std::string_view role) {
  std::random_device random;
  std::array<std::uint32_t, 4> words{};
  for (auto &word : words) {
    word = random();
  }
  std::ostringstream stream;
  stream << role << '-';
  for (const auto word : words) {
    stream << std::hex << std::setfill('0') << std::setw(8) << word;
  }
  return stream.str();
}

bool same_execution_environment(const DispatchAuthorityPin &expected,
                                const DispatchAuthorityPin &observed) {
  return expected.binding_contract_generation_id ==
             observed.binding_contract_generation_id &&
         expected.binding_activity_generation ==
             observed.binding_activity_generation &&
         expected.lifecycle_generation == observed.lifecycle_generation &&
         expected.runtime_generation_id == observed.runtime_generation_id &&
         expected.provider_component_epoch ==
             observed.provider_component_epoch &&
         expected.transport_owner_component_epoch ==
             observed.transport_owner_component_epoch &&
         expected.authority_component_epoch ==
             observed.authority_component_epoch &&
         expected.cell_supervisor_component_epoch ==
             observed.cell_supervisor_component_epoch &&
         expected.execution_backend_component_epoch ==
             observed.execution_backend_component_epoch;
}

core::Duration gripper_duration_ns(double seconds) noexcept {
  constexpr double kNanosecondsPerSecond = 1'000'000'000.0;
  const double nanoseconds = seconds * kNanosecondsPerSecond;
  if (!std::isfinite(nanoseconds) ||
      nanoseconds >=
          static_cast<double>(std::numeric_limits<core::Duration>::max())) {
    return std::numeric_limits<core::Duration>::max();
  }
  if (nanoseconds <= 0.0) {
    return 0;
  }
  return static_cast<core::Duration>(nanoseconds);
}

core::TimePoint gripper_deadline(core::TimePoint started_at,
                                 core::Duration duration) noexcept {
  const auto maximum = std::numeric_limits<core::TimePoint>::max();
  if (duration > 0 && started_at > maximum - duration) {
    return maximum;
  }
  return started_at + duration;
}

core::Duration gripper_duration_sum(core::Duration lhs,
                                    core::Duration rhs) noexcept {
  const auto maximum = std::numeric_limits<core::Duration>::max();
  return lhs > maximum - rhs ? maximum : lhs + rhs;
}

std::int64_t ros_stamp_ns(const builtin_interfaces::msg::Time &stamp) noexcept {
  return static_cast<std::int64_t>(stamp.sec) * 1'000'000'000LL +
         static_cast<std::int64_t>(stamp.nanosec);
}

double controlled_stop_wait_timeout_s(
    const core::MotionInterruptStopProfileDTO &profile,
    double stop_completion_tolerance_s) {
  return core::motion_interrupt_deceleration_time_or_default(profile) +
         std::max(0.0, stop_completion_tolerance_s);
}

core::InterruptStructuredValue
structured_text_details(const std::map<std::string, std::string> &details) {
  core::InterruptStructuredValue structured;
  for (const auto &[key, value] : details) {
    structured.emplace(key, core::interrupt_structured_string(value));
  }
  return structured;
}

std::optional<std::vector<double>> angular_settle_velocity_thresholds(
    const core::MotionInterruptStopProfileDTO &profile,
    const std::vector<std::string> &joint_names, std::string &failure_reason) {
  std::vector<double> thresholds(joint_names.size(),
                                 core::kDefaultSettleVelocityThresholdRadS);
  if (!profile.per_joint_settle_velocity_threshold.has_value()) {
    return thresholds;
  }

  for (const auto &[source_joint, typed_threshold] :
       *profile.per_joint_settle_velocity_threshold) {
    const auto joint =
        std::find(joint_names.begin(), joint_names.end(), source_joint);
    if (joint == joint_names.end()) {
      failure_reason = "settle_velocity_threshold_unknown_source_joint";
      return std::nullopt;
    }
    const auto *angular =
        std::get_if<core::AngularVelocityDTO>(&typed_threshold);
    if (angular == nullptr || !std::isfinite(angular->value_rad_s) ||
        angular->value_rad_s <= 0.0) {
      failure_reason = "settle_velocity_threshold_coordinate_kind_mismatch";
      return std::nullopt;
    }
    thresholds[static_cast<std::size_t>(joint - joint_names.begin())] =
        angular->value_rad_s;
  }
  return thresholds;
}

bool angular_stop_acceleration_scope_matches(
    const core::MotionInterruptStopProfileDTO &profile,
    const std::vector<std::string> &joint_names, std::string &failure_reason) {
  if (profile.stop_mode == core::MotionInterruptStopMode::kHardDeceleration) {
    return true;
  }
  if (profile.per_joint_max_deceleration.size() != joint_names.size()) {
    failure_reason = "stop_acceleration_scope_coverage_mismatch";
    return false;
  }
  for (const auto &joint_name : joint_names) {
    const auto limit = profile.per_joint_max_deceleration.find(joint_name);
    if (limit == profile.per_joint_max_deceleration.end()) {
      failure_reason = "stop_acceleration_scope_coverage_mismatch";
      return false;
    }
    const auto *angular =
        std::get_if<core::AngularAccelerationDTO>(&limit->second);
    if (angular == nullptr || !std::isfinite(angular->value_rad_s2) ||
        angular->value_rad_s2 <= 0.0) {
      failure_reason = "stop_acceleration_coordinate_kind_mismatch";
      return false;
    }
  }
  return true;
}

double clamp_progress(double progress) {
  return std::max(0.0, std::min(1.0, progress));
}

void atomic_max(std::atomic<std::uint64_t> &target, std::uint64_t value) {
  auto current = target.load(std::memory_order_relaxed);
  while (current < value && !target.compare_exchange_weak(
                                current, value, std::memory_order_relaxed,
                                std::memory_order_relaxed)) {
  }
}

void atomic_max_sequentially_consistent(std::atomic<std::uint64_t> &target,
                                        std::uint64_t value) {
  auto current = target.load(std::memory_order_seq_cst);
  while (current < value && !target.compare_exchange_weak(
                                current, value, std::memory_order_seq_cst,
                                std::memory_order_seq_cst)) {
  }
}

void record_latency_sample(std::atomic<std::uint64_t> &last,
                           std::atomic<std::uint64_t> &maximum,
                           std::atomic<std::uint64_t> &count,
                           std::int64_t latency_ns) {
  const auto clamped =
      static_cast<std::uint64_t>(std::max<std::int64_t>(0, latency_ns));
  last.store(clamped, std::memory_order_relaxed);
  atomic_max(maximum, clamped);
  count.fetch_add(1, std::memory_order_relaxed);
}

void record_latency_value(std::atomic<std::uint64_t> &last,
                          std::atomic<std::uint64_t> &maximum,
                          std::int64_t latency_ns) {
  const auto clamped =
      static_cast<std::uint64_t>(std::max<std::int64_t>(0, latency_ns));
  last.store(clamped, std::memory_order_relaxed);
  atomic_max(maximum, clamped);
}

std::string format_double(double value) {
  std::ostringstream out;
  out << std::setprecision(17) << value;
  return out.str();
}

std::string controller_manager_endpoint(const std::string &manager,
                                        std::string_view endpoint) {
  std::string result = manager;
  while (result.size() > 1 && result.back() == '/') {
    result.pop_back();
  }
  if (result == "/") {
    return result + std::string(endpoint);
  }
  return result + "/" + std::string(endpoint);
}

core::ErrorDTO make_error(core::ErrorCode code, std::string message) {
  return core::ErrorDTO{
      std::string(core::to_string(code)), std::move(message), {}};
}

core::ErrorDTO make_error(core::ErrorCode code, std::string message,
                          std::map<std::string, std::string> details) {
  return core::ErrorDTO{std::string(core::to_string(code)), std::move(message),
                        std::move(details)};
}

core::ParticipantStopStageResultDTO
unresolved_stop_stage_result(const core::ResolvedParticipantStopStageDTO &stage,
                             core::ParticipantStopStageOutcomeDTO outcome,
                             std::string code, std::string message,
                             std::string reason) {
  const core::ErrorDTO error{
      std::move(code), std::move(message), {{"reason", std::move(reason)}}};
  core::ParticipantStopStageResultDTO result{
      stage,        core::StopStageAttemptDispositionDTO::kNotDispatched,
      outcome,      std::nullopt,
      std::nullopt, std::nullopt,
      std::nullopt, std::nullopt,
      std::nullopt, 0,
      {},           error};
  if (stage.stage_index >= stage.participant_stop_plan.stage_commands.size()) {
    return result;
  }
  const auto &requirements =
      stage.participant_stop_plan.stage_commands[stage.stage_index]
          .resolved_stage.evidence_requirements;
  result.evidence_items.reserve(requirements.size());
  for (const auto &requirement : requirements) {
    result.evidence_items.push_back(core::StopEvidenceItemDTO{
        requirement.requirement.evidence_item_id,
        requirement.requirement.evidence_kind,
        outcome == core::ParticipantStopStageOutcomeDTO::kSupersededNoEffect
            ? core::StopEvidenceItemStateDTO::kInapplicable
            : core::StopEvidenceItemStateDTO::kMissing,
        requirement.resolved_source, std::nullopt, std::nullopt, std::nullopt,
        std::nullopt, error});
  }
  return result;
}

builtin_interfaces::msg::Duration duration_from_seconds(double seconds) {
  builtin_interfaces::msg::Duration duration;
  const double clamped = std::max(0.0, seconds);
  const auto whole = static_cast<int32_t>(std::floor(clamped));
  const auto nanosec = static_cast<uint32_t>(
      std::round((clamped - static_cast<double>(whole)) * 1e9));
  duration.sec = whole;
  duration.nanosec = std::min<uint32_t>(nanosec, 999999999u);
  return duration;
}

double seconds_since(std::chrono::steady_clock::time_point t,
                     std::chrono::steady_clock::time_point now) {
  return std::chrono::duration<double>(now - t).count();
}

template <typename ActionT>
bool wait_for_action_server_until(
    const std::shared_ptr<rclcpp_action::Client<ActionT>> &client,
    double timeout_s) {
  if (!client) {
    return false;
  }
  const auto budget =
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::duration<double>(std::max(0.0, timeout_s)));
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (true) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      return client->action_server_is_ready();
    }
    // Graph discovery can wake a wait before every action endpoint is ready.
    // Retry bounded slices until the caller's single overall deadline.
    const auto remaining = deadline - now;
    const auto slice = std::min(
        remaining,
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::milliseconds(100)));
    if (client->wait_for_action_server(
            std::chrono::duration_cast<std::chrono::nanoseconds>(slice))) {
      return true;
    }
  }
}

void add_unique_evidence(std::vector<std::string> &evidence,
                         const std::string &label) {
  if (label.empty()) {
    return;
  }
  if (std::find(evidence.begin(), evidence.end(), label) == evidence.end()) {
    evidence.push_back(label);
  }
}

core::GripperCommandResultDTO
rejected_gripper_command(std::string reason_code, std::string message,
                         std::uint64_t command_id = 0,
                         std::uint64_t generation = 0) {
  return core::GripperCommandResultDTO{
      false, command_id, generation,
      core::ErrorDTO{std::move(reason_code), std::move(message), {}}};
}

void disable_fjt_controller_tolerances(
    control_msgs::action::FollowJointTrajectory::Goal &goal,
    const std::vector<std::string> &joint_names) {
  goal.path_tolerance.clear();
  goal.goal_tolerance.clear();
  for (const auto &joint_name : joint_names) {
    control_msgs::msg::JointTolerance tolerance;
    tolerance.name = joint_name;
    tolerance.position = -1.0;
    tolerance.velocity = -1.0;
    tolerance.acceleration = -1.0;
    goal.path_tolerance.push_back(tolerance);
    goal.goal_tolerance.push_back(std::move(tolerance));
  }
}

} // namespace

Ros2ControlExecutionBackend::Ros2ControlExecutionBackend(
    std::shared_ptr<core::IExecutionStateSink> execution_state_sink,
    std::string group_name, core::IClock &clock, core::IScheduler &scheduler,
    Ros2ControlExecutionBackendConfig config)
    : Ros2ControlExecutionBackend(
          false, nullptr, std::move(execution_state_sink),
          std::move(group_name), clock, scheduler, std::move(config)) {}

Ros2ControlExecutionBackend::Ros2ControlExecutionBackend(
    DeferredRosExecutorStartTag,
    std::shared_ptr<core::IExecutionStateSink> execution_state_sink,
    std::string group_name, core::IClock &clock, core::IScheduler &scheduler,
    Ros2ControlExecutionBackendConfig config)
    : Ros2ControlExecutionBackend(
          true, nullptr, std::move(execution_state_sink), std::move(group_name),
          clock, scheduler, std::move(config)) {}

Ros2ControlExecutionBackend::Ros2ControlExecutionBackend(
    DeferredRosExecutorStartTag,
    core::ObservedRobotStatePublication &addressed_state_publication,
    std::shared_ptr<core::IExecutionStateSink> execution_state_sink,
    std::string group_name, core::IClock &clock, core::IScheduler &scheduler,
    Ros2ControlExecutionBackendConfig config)
    : Ros2ControlExecutionBackend(
          true, &addressed_state_publication, std::move(execution_state_sink),
          std::move(group_name), clock, scheduler, std::move(config)) {}

Ros2ControlExecutionBackend::Ros2ControlExecutionBackend(
    bool defer_ros_executor_start,
    core::ObservedRobotStatePublication *addressed_state_publication,
    std::shared_ptr<core::IExecutionStateSink> execution_state_sink,
    std::string group_name, core::IClock &clock, core::IScheduler &scheduler,
    Ros2ControlExecutionBackendConfig config)
    : execution_state_sink_(std::move(execution_state_sink)),
      group_name_(std::move(group_name)), clock_(clock), scheduler_(scheduler),
      config_(std::move(config)),
      execution_backend_component_epoch_(
          new_component_epoch("execution-backend")) {
  if (!execution_state_sink_) {
    throw std::invalid_argument("execution state sink must not be null");
  }
  validate_config();
  initialize_prepared_participant_bindings();
  if (addressed_state_publication == nullptr) {
    initialize_observed_state_publication();
  } else {
    observed_state_publication_ = addressed_state_publication;
    validate_injected_observed_state_publication();
  }
  initialize_execution_progress_publication();
  initialize_scene_projection_worker();
  initialize_dispatch_admission();
  snapshot_.timestamp_ns = clock_.now();
  initialize_authored_state();
  initialize_ros();
  if (!defer_ros_executor_start) {
    complete_ros_configuration();
  }
}

void Ros2ControlExecutionBackend::initialize_prepared_participant_bindings() {
  prepared_participant_aliases_.store(
      std::make_shared<const std::vector<core::RuntimeParticipantBinding>>(),
      std::memory_order_release);
  if (config_.prepared_participant_binding.has_value()) {
    prepared_participant_binding_.store(
        std::make_shared<const core::RuntimeParticipantBinding>(
            *config_.prepared_participant_binding),
        std::memory_order_release);
  }
}

Ros2ControlExecutionBackend::~Ros2ControlExecutionBackend() {
  GoalHandleFJT::SharedPtr handle_to_cancel;
  GoalHandleFJT::SharedPtr managed_fjt_handle_to_cancel;
  {
    std::lock_guard<std::mutex> lock(execution_mutex_);
    handle_to_cancel = active_goal_handle_;
    set_running_locked(false);
    terminal_ = true;
    stop_requested_ = true;
    natural_completion_stop_requested_ = false;
    pause_requested_ = false;
    active_goal_handle_.reset();
    active_goal_id_.reset();
  }
  {
    std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
    managed_fjt_handle_to_cancel = managed_fjt_goal_;
    managed_fjt_goal_.reset();
    ++managed_fjt_generation_;
    forced_managed_stop_execution_generation_.reset();
    forced_managed_stop_generation_.reset();
    managed_fjt_terminal_goal_id_.reset();
    managed_fjt_terminal_result_received_ = true;
    managed_fjt_terminal_result_success_ = false;
    managed_fjt_terminal_error_ = make_error(
        core::ErrorCode::kStopFailed,
        "managed controlled stop was preempted by backend destruction",
        {{"reason", "managed_stop_backend_destroyed"}});
    managed_fjt_cv_.notify_all();
  }
  cancel_goal_if_known(handle_to_cancel);
  cancel_goal_if_known(managed_fjt_handle_to_cancel);
  invalidate_gripper_commands(core::GripperInvalidationReason::kShutdown);
  if (!scheduler_.drain_for(core::SchedulerPriority::ControllerCommand,
                            std::chrono::seconds(1))) {
    RCLCPP_FATAL(node_->get_logger(),
                 "Timed out draining gripper controller-command callbacks "
                 "during backend destruction");
    std::terminate();
  }
  set_command_gate(false);
  execution_cv_.notify_all();
  if (monitor_thread_.joinable()) {
    monitor_thread_.join();
  }
  stop_ros();
  stop_scene_projection_worker();
}

void Ros2ControlExecutionBackend::validate_config() const {
  if (config_.action_name.empty()) {
    throw std::invalid_argument("ros2_control action_name must not be empty");
  }
  if (config_.joint_state_topic.empty() &&
      config_.controller_state_topic.empty()) {
    throw std::invalid_argument("ros2_control execution requires "
                                "joint_state_topic or controller_state_topic");
  }
  if (config_.required_joints.empty()) {
    throw std::invalid_argument(
        "ros2_control required_joints must not be empty");
  }
  const auto &transport_joints = transport_joint_names();
  const std::set<std::string> unique_transport_joints(transport_joints.begin(),
                                                      transport_joints.end());
  if (transport_joints.size() != config_.required_joints.size() ||
      unique_transport_joints.size() != transport_joints.size() ||
      std::any_of(transport_joints.begin(), transport_joints.end(),
                  [](const auto &joint) { return joint.empty(); })) {
    throw std::invalid_argument(
        "ros2_control transport_joint_names must be unique, non-empty, and "
        "match required_joints cardinality");
  }
  if (config_.prepared_participant_binding.has_value()) {
    const auto &binding = config_.prepared_participant_binding->value();
    if (!binding.control_path.has_value()) {
      throw std::invalid_argument(
          "prepared participant binding requires one control path");
    }
    std::vector<std::string> binding_joints;
    std::vector<std::string> binding_transport_joints;
    binding_joints.reserve(binding.resources.size());
    binding_transport_joints.reserve(binding.resources.size());
    for (const auto &resource : binding.resources) {
      if (!resource.source_joint_name.has_value() ||
          resource.compiled_resource_name.empty()) {
        throw std::invalid_argument(
            "prepared ros2_control binding requires model-backed compiled "
            "joint names");
      }
      binding_joints.push_back(resource.compiled_resource_name);
      binding_transport_joints.push_back(*resource.source_joint_name);
    }
    if (binding_joints != config_.required_joints ||
        binding_transport_joints != transport_joints) {
      throw std::invalid_argument(
          "prepared ros2_control binding does not match its compiled and "
          "transport joint names");
    }
  }
  if (config_.observed_state_source_precedence.empty()) {
    throw std::invalid_argument(
        "observed_state_source_precedence must not be empty");
  }
  const std::array<std::string_view, 3> allowed_sources{
      "action_feedback", "controller_state", "joint_state"};
  for (std::size_t i = 0; i < config_.observed_state_source_precedence.size();
       ++i) {
    const auto &source = config_.observed_state_source_precedence[i];
    if (std::find(allowed_sources.begin(), allowed_sources.end(), source) ==
            allowed_sources.end() ||
        std::find(config_.observed_state_source_precedence.begin(),
                  config_.observed_state_source_precedence.begin() +
                      static_cast<std::ptrdiff_t>(i),
                  source) != config_.observed_state_source_precedence.begin() +
                                 static_cast<std::ptrdiff_t>(i)) {
      throw std::invalid_argument("observed_state_source_precedence contains "
                                  "an unknown or duplicate source");
    }
  }
  if (!uses_authored_backend_state() &&
      std::none_of(config_.observed_state_source_precedence.begin(),
                   config_.observed_state_source_precedence.end(),
                   [this](const std::string &source) {
                     return (source == "controller_state" &&
                             !config_.controller_state_topic.empty()) ||
                            (source == "joint_state" &&
                             !config_.joint_state_topic.empty());
                   })) {
    throw std::invalid_argument(
        "observed_ros2 completion requires a configured controller_state or "
        "joint_state topic in observed_state_source_precedence");
  }
  if (config_.joint_state_max_age_s < 0.0 ||
      config_.wait_for_state_timeout_s < 0.0 ||
      config_.wait_for_action_server_timeout_s < 0.0 ||
      config_.start_tolerance_rad < 0.0 || config_.final_tolerance_rad < 0.0 ||
      config_.tracking_error_tolerance_rad < 0.0 ||
      config_.tracking_error_timeout_s < 0.0 ||
      !std::isfinite(config_.controller_binding_refresh_timeout_s) ||
      config_.controller_binding_refresh_timeout_s <= 0.0 ||
      !std::isfinite(config_.execution_progress_projection_period_s) ||
      config_.execution_progress_projection_period_s <= 0.0 ||
      !std::isfinite(
          config_.execution_progress_projection_max_rms_distance_rad) ||
      config_.execution_progress_projection_max_rms_distance_rad < 0.0 ||
      !std::isfinite(
          config_.execution_progress_projection_ambiguity_epsilon_rad) ||
      config_.execution_progress_projection_ambiguity_epsilon_rad < 0.0 ||
      !std::isfinite(config_.execution_progress_projection_ambiguity_time_s) ||
      config_.execution_progress_projection_ambiguity_time_s < 0.0 ||
      !std::isfinite(
          config_.execution_progress_projection_regression_tolerance_s) ||
      config_.execution_progress_projection_regression_tolerance_s < 0.0 ||
      config_.stop_completion_timeout_s < 0.0 ||
      config_.sim_reset_motion_duration_s < 0.0 ||
      !std::isfinite(config_.soft_on_path_stop_traj_delta_s) ||
      config_.soft_on_path_stop_traj_delta_s < 0.0 ||
      !std::isfinite(config_.soft_on_path_sample_period_s) ||
      config_.soft_on_path_sample_period_s <= 0.0 ||
      config_.monitor_period_s <= 0.0) {
    throw std::invalid_argument(
        "ros2_control timing/tolerance config is invalid");
  }
  if (config_.executor_thread_count < 3) {
    throw std::invalid_argument(
        "ros2_control executor_thread_count must be >= 3 so action, state, "
        "and default callback groups can progress independently");
  }
  if (config_.soft_on_path_max_initial_path_deviation_rad.has_value() &&
      (!std::isfinite(*config_.soft_on_path_max_initial_path_deviation_rad) ||
       *config_.soft_on_path_max_initial_path_deviation_rad < 0.0)) {
    throw std::invalid_argument(
        "soft_on_path_max_initial_path_deviation_rad must be finite and >= 0");
  }
  if (config_.soft_on_path_max_initial_velocity_deviation_rad_s.has_value() &&
      (!std::isfinite(
           *config_.soft_on_path_max_initial_velocity_deviation_rad_s) ||
       *config_.soft_on_path_max_initial_velocity_deviation_rad_s < 0.0)) {
    throw std::invalid_argument("soft_on_path_max_initial_velocity_deviation_"
                                "rad_s must be finite and >= 0");
  }
  for (const auto &[joint, jerk] : config_.soft_on_path_max_jerk_rad_s3) {
    if (joint.empty() || !std::isfinite(jerk) || jerk <= 0.0) {
      throw std::invalid_argument(
          "soft_on_path_max_jerk_rad_s3 entries must have non-empty joint "
          "names and finite positive values");
    }
  }
  if (config_.execution_state_source_mode != "authored_preview" &&
      config_.execution_state_source_mode != "commanded_sim" &&
      config_.execution_state_source_mode != "observed_ros2" &&
      config_.execution_state_source_mode != "observed_robot") {
    throw std::invalid_argument(
        "execution_state_source_mode must be one of authored_preview, "
        "commanded_sim, observed_ros2");
  }
  if (config_.execution_state_source_mode == "commanded_sim" &&
      config_.initial_joint_positions_rad.size() !=
          config_.required_joints.size()) {
    throw std::invalid_argument(
        "commanded_sim requires initial_joint_positions_rad with one value per "
        "required joint");
  }
  if (config_.disable_controller_path_tolerances &&
      config_.execution_state_source_mode != "commanded_sim") {
    throw std::invalid_argument("disable_controller_path_tolerances is allowed "
                                "only for commanded_sim deployments");
  }
  if (config_.require_execution_admission_snapshot &&
      !config_.controller_binding.has_value()) {
    throw std::invalid_argument(
        "require_execution_admission_snapshot requires a controller binding");
  }
  if (config_.controller_binding.has_value()) {
    const auto &binding = *config_.controller_binding;
    const auto empty = [](const std::string &value) { return value.empty(); };
    if (binding.generation_id.empty() || binding.controller_manager.empty() ||
        binding.activity_topic.empty() ||
        binding.hardware_component_name.empty() ||
        binding.trajectory_controller_manager_name.empty() ||
        binding.trajectory_controller_ros_name.empty() ||
        binding.trajectory_controller_type.empty() ||
        binding.follow_joint_trajectory_action.empty() ||
        binding.controller_state_topic.empty() ||
        binding.joint_state_topic.empty() || binding.required_joints.empty() ||
        binding.claimed_command_interfaces.empty() ||
        binding.required_state_interfaces.empty() ||
        binding.snapshot_max_age_ns <= 0 ||
        std::any_of(binding.required_joints.begin(),
                    binding.required_joints.end(), empty) ||
        std::any_of(binding.claimed_command_interfaces.begin(),
                    binding.claimed_command_interfaces.end(), empty) ||
        std::any_of(binding.required_state_interfaces.begin(),
                    binding.required_state_interfaces.end(), empty)) {
      throw std::invalid_argument("controller binding contract is incomplete");
    }
    const auto has_duplicates = [](const std::vector<std::string> &values) {
      auto copy = values;
      std::sort(copy.begin(), copy.end());
      return std::adjacent_find(copy.begin(), copy.end()) != copy.end();
    };
    if (has_duplicates(binding.required_joints) ||
        has_duplicates(binding.claimed_command_interfaces) ||
        has_duplicates(binding.required_state_interfaces)) {
      throw std::invalid_argument(
          "controller binding contract contains duplicate names");
    }
    if (binding.required_joints != transport_joints ||
        binding.follow_joint_trajectory_action != config_.action_name ||
        binding.controller_state_topic != config_.controller_state_topic ||
        binding.joint_state_topic != config_.joint_state_topic) {
      throw std::invalid_argument(
          "controller binding disagrees with execution endpoint/joint config");
    }
  }
}

core::PrepareParticipantResultDTO Ros2ControlExecutionBackend::Prepare(
    const core::PreparedOperationParticipantDTO &participant) {
  const auto reject = [&](std::string reason) {
    auto result = core::MakePrepareParticipantResult(
        participant, core::PrepareParticipantResultStateDTO::kRejected,
        core::ErrorDTO{core::kErrorExecutionRejected,
                       "ros2_control backend rejected prepared participant",
                       {{"reason", std::move(reason)}}});
    if (!result.ok()) {
      throw std::invalid_argument(result.error().message);
    }
    return std::move(result.value());
  };
  std::lock_guard<std::mutex> lock(prepared_execution_mutex_);
  const auto retained_binding =
      prepared_participant_binding_.load(std::memory_order_acquire);
  if (!retained_binding) {
    return reject("prepared_backend_binding_unavailable");
  }
  const auto binding_error =
      [&](const core::RuntimeParticipantBinding &binding) {
        return core::prepared_participant_backend_binding_field_error(
            participant, binding,
            core::PreparedParticipantPayloadKindDTO::kMotionExecution);
      };
  auto field_error = binding_error(*retained_binding);
  if (field_error.has_value()) {
    const auto aliases =
        prepared_participant_aliases_.load(std::memory_order_acquire);
    const auto matched =
        std::find_if(aliases->begin(), aliases->end(), [&](const auto &alias) {
          return !binding_error(alias).has_value();
        });
    if (matched == aliases->end()) {
      return reject(std::string(*field_error));
    }
  }
  const auto &motion =
      std::get<core::PreparedMotionExecutionDTO>(participant.payload);
  if (motion.trajectory.resources != participant.resources ||
      motion.trajectory.joint_names != config_.required_joints) {
    return reject("prepared_trajectory_scope_mismatch");
  }

  const auto key =
      std::make_tuple(participant.operation_id, participant.operation_revision,
                      participant.participant_id);
  const auto found = prepared_participants_.find(key);
  if (found != prepared_participants_.end()) {
    if (found->second != participant.prepared_contract_digest) {
      return reject("prepared_contract_conflict");
    }
  } else {
    if (prepared_participants_.size() >= kPreparedReplayCapacity) {
      return reject("prepared_replay_capacity_exhausted");
    }
    prepared_participants_.emplace(key, participant.prepared_contract_digest);
  }
  auto result = core::MakePrepareParticipantResult(
      participant, core::PrepareParticipantResultStateDTO::kPrepared);
  if (!result.ok()) {
    throw std::invalid_argument(result.error().message);
  }
  return std::move(result.value());
}

core::ExecutionHandle Ros2ControlExecutionBackend::dispatch_prepared(
    const core::PreparedOperationParticipantDTO &participant,
    const core::ControllerGoalId &controller_goal_id,
    core::ControllerGoalGeneration controller_goal_generation,
    bool resume_dispatch) {
  const auto rejected = [&](std::string reason) {
    core::ExecutionSnapshot snapshot;
    snapshot.timestamp_ns = clock_.now();
    snapshot.state = core::ExecutionState::kFailed;
    snapshot.error =
        core::ErrorDTO{core::kErrorExecutionRejected,
                       "ros2_control backend rejected prepared dispatch",
                       {{"reason", std::move(reason)}}};
    core::ExecutionHandle handle{
        [](std::int64_t) { return true; },
        [snapshot = std::move(snapshot)]() { return snapshot; }, 0U};
    handle.controller_goal_id = controller_goal_id;
    handle.controller_goal_generation = controller_goal_generation;
    handle.prepared_contract_digest = participant.prepared_contract_digest;
    return handle;
  };

  std::lock_guard<std::mutex> lock(prepared_execution_mutex_);
  if (controller_goal_id.empty() || !controller_goal_generation.nonzero()) {
    return rejected("controller_goal_identity_invalid");
  }
  const auto retained_binding =
      prepared_participant_binding_.load(std::memory_order_acquire);
  if (!retained_binding) {
    return rejected("prepared_backend_binding_unavailable");
  }
  const auto binding_error =
      [&](const core::RuntimeParticipantBinding &binding) {
        return core::prepared_participant_backend_binding_field_error(
            participant, binding,
            core::PreparedParticipantPayloadKindDTO::kMotionExecution);
      };
  auto field_error = binding_error(*retained_binding);
  if (field_error.has_value()) {
    const auto aliases =
        prepared_participant_aliases_.load(std::memory_order_acquire);
    const auto matched =
        std::find_if(aliases->begin(), aliases->end(), [&](const auto &alias) {
          return !binding_error(alias).has_value();
        });
    if (matched == aliases->end()) {
      return rejected(std::string(*field_error));
    }
  }
  const auto prepared_key =
      std::make_tuple(participant.operation_id, participant.operation_revision,
                      participant.participant_id);
  const auto prepared = prepared_participants_.find(prepared_key);
  if (prepared == prepared_participants_.end() ||
      prepared->second != participant.prepared_contract_digest) {
    return rejected("prepared_participant_not_admitted");
  }
  const auto replay = prepared_goals_.find(controller_goal_id);
  if (replay != prepared_goals_.end()) {
    if (replay->second.generation == controller_goal_generation &&
        replay->second.prepared_contract_digest ==
            participant.prepared_contract_digest) {
      return replay->second.handle;
    }
    return rejected("controller_goal_identity_conflict");
  }
  if (prepared_goals_.size() >= kPreparedReplayCapacity) {
    return rejected("controller_goal_replay_capacity_exhausted");
  }

  const auto &trajectory =
      std::get<core::PreparedMotionExecutionDTO>(participant.payload)
          .trajectory;
  auto handle =
      resume_dispatch ? resume(trajectory) : execute_trajectory(trajectory);
  handle.controller_goal_id = controller_goal_id;
  handle.controller_goal_generation = controller_goal_generation;
  handle.prepared_contract_digest = participant.prepared_contract_digest;
  std::vector<core::PreparedParticipantStopPlanDTO> stop_plans;
  stop_plans.push_back(participant.ordinary_stop_plan);
  for (const auto &scope :
       std::get<core::PreparedMotionExecutionDTO>(participant.payload)
           .interrupt_scopes) {
    stop_plans.push_back(scope.stop_plan);
  }
  prepared_goals_.emplace(
      controller_goal_id,
      PreparedGoalRecord{controller_goal_generation,
                         participant.prepared_contract_digest,
                         participant.operation_id,
                         participant.operation_revision,
                         participant.participant_id,
                         participant.controllable_component_id,
                         participant.command_target_id,
                         participant.capability_id,
                         participant.control_domain_id,
                         participant.control_domain_revision,
                         participant.control_binding_id,
                         participant.control_binding_revision,
                         participant.controller_topology_id,
                         participant.topology_revision,
                         participant.control_route_id,
                         participant.active_control_configuration_id,
                         participant.active_control_configuration_generation,
                         participant.control_provider.runtime_epoch,
                         participant.resources,
                         participant.stop_evidence_policy_id,
                         participant.stop_evidence_policy_revision,
                         std::move(stop_plans),
                         handle});
  return handle;
}

core::ExecutionHandle Ros2ControlExecutionBackend::Dispatch(
    const core::PreparedOperationParticipantDTO &participant,
    const core::ControllerGoalId &controller_goal_id,
    core::ControllerGoalGeneration controller_goal_generation) {
  return dispatch_prepared(participant, controller_goal_id,
                           controller_goal_generation, false);
}

core::ExecutionHandle Ros2ControlExecutionBackend::DispatchResume(
    const core::PreparedOperationParticipantDTO &participant,
    const core::ControllerGoalId &controller_goal_id,
    core::ControllerGoalGeneration controller_goal_generation) {
  return dispatch_prepared(participant, controller_goal_id,
                           controller_goal_generation, true);
}

core::Result<void> Ros2ControlExecutionBackend::RebindRoute(
    core::RuntimeParticipantBinding binding,
    std::vector<core::RuntimeParticipantBinding> program_aliases) {
  const auto error = [](core::ErrorCode code, std::string reason,
                        std::string message) {
    return core::Result<void>::Err(
        core::ErrorDTO{std::string(core::to_string(code)),
                       std::move(message),
                       {{"reason", std::move(reason)}}});
  };
  const auto &value = binding.value();
  std::vector<std::string> binding_joints;
  std::vector<std::string> binding_transport_joints;
  binding_joints.reserve(value.resources.size());
  binding_transport_joints.reserve(value.resources.size());
  if (!value.control_path.has_value() || value.resources.empty() ||
      std::any_of(
          value.resources.begin(), value.resources.end(),
          [&](const auto &resource) {
            if (!resource.source_joint_name.has_value() ||
                resource.compiled_resource_name.empty() ||
                resource.coordinate_kind != core::CoordinateKind::kAngular) {
              return true;
            }
            binding_joints.push_back(resource.compiled_resource_name);
            binding_transport_joints.push_back(*resource.source_joint_name);
            return false;
          }) ||
      binding_joints != config_.required_joints ||
      binding_transport_joints != transport_joint_names()) {
    return error(core::ErrorCode::kInvalidRequest,
                 "ros2_control_motion_route_rebind_mismatch",
                 "ros2_control route publication crosses its configured "
                 "motion resources");
  }
  if (program_aliases.size() > 64U ||
      std::any_of(
          program_aliases.begin(), program_aliases.end(),
          [&](const auto &alias) {
            return !core::runtime_participant_binding_physically_contains(
                binding, alias);
          })) {
    return error(core::ErrorCode::kInvalidRequest,
                 "ros2_control_motion_program_alias_mismatch",
                 "ros2_control Program aliases cross the physical backend");
  }
  try {
    auto retained = std::make_shared<const core::RuntimeParticipantBinding>(
        std::move(binding));
    auto retained_aliases =
        std::make_shared<const std::vector<core::RuntimeParticipantBinding>>(
            std::move(program_aliases));
    std::lock_guard<std::mutex> prepared_lock(prepared_execution_mutex_);
    {
      std::lock_guard<std::mutex> execution_lock(execution_mutex_);
      if (running_ || !terminal_) {
        return error(core::ErrorCode::kExecutionRejected,
                     "ros2_control_motion_route_rebind_busy",
                     "ros2_control route publication cannot rebind an active "
                     "motion backend");
      }
    }
    prepared_participants_.clear();
    prepared_goals_.clear();
    prepared_participant_binding_.store(std::move(retained),
                                        std::memory_order_release);
    prepared_participant_aliases_.store(std::move(retained_aliases),
                                        std::memory_order_release);
    return core::Result<void>::Ok();
  } catch (...) {
    return error(core::ErrorCode::kInternalError,
                 "ros2_control_motion_route_rebind_capacity_unavailable",
                 "ros2_control route publication could not retain the motion "
                 "binding");
  }
}

void Ros2ControlExecutionBackend::initialize_observed_state_publication() {
  if (uses_authored_backend_state()) {
    authored_at_rest_velocities_.assign(config_.required_joints.size(), 0.0);
  }
  owned_observed_state_publication_ =
      std::make_unique<core::ObservedRobotStatePublication>(
          config_.required_joints,
          ros2_control_observed_state_sources(config_));
  observed_state_publication_ = owned_observed_state_publication_.get();
}

void Ros2ControlExecutionBackend::
    validate_injected_observed_state_publication() {
  if (observed_state_publication_ == nullptr ||
      !std::equal(observed_state_publication_->canonical_joint_names().begin(),
                  observed_state_publication_->canonical_joint_names().end(),
                  config_.required_joints.begin(),
                  config_.required_joints.end()) ||
      observed_state_publication_->source_configs() !=
          ros2_control_addressed_state_sources(config_)) {
    throw std::invalid_argument(
        "injected observed-state publication differs from the backend config");
  }
  if (uses_authored_backend_state()) {
    authored_at_rest_velocities_.assign(config_.required_joints.size(), 0.0);
  }
  const auto diagnostic_sources =
      ros2_control_diagnostic_state_sources(config_);
  if (!diagnostic_sources.empty()) {
    diagnostic_observed_state_publication_ =
        std::make_unique<core::ObservedRobotStatePublication>(
            config_.required_joints, diagnostic_sources);
  }
}

void Ros2ControlExecutionBackend::initialize_execution_progress_publication() {
  const bool commanded = uses_authored_backend_state();
  const auto freshness_budget_ns =
      commanded ? std::int64_t{0}
                : static_cast<std::int64_t>(config_.joint_state_max_age_s *
                                            1'000'000'000.0);
  execution_progress_publication_ =
      std::make_unique<core::ExecutionProgressPublication>(
          commanded ? core::ExecutionProgressSourceKind::kBackendCommanded
                    : core::ExecutionProgressSourceKind::kMeasuredProjection,
          commanded
              ? core::ExecutionProgressAuthorityProfile::kCommandedSimulation
              : core::ExecutionProgressAuthorityProfile::kMeasuredPhysical,
          commanded ? "ros2_control_commanded_progress_v1"
                    : "ros2_control_measured_progress_v1",
          freshness_budget_ns);
}

void Ros2ControlExecutionBackend::initialize_scene_projection_worker() {
  scene_projection_worker_.reset(new detail::SceneProjectionWorker(
      execution_state_sink_, group_name_, clock_));
}

void Ros2ControlExecutionBackend::initialize_dispatch_admission() {
  controller_binding_snapshot_.store(nullptr, std::memory_order_release);
  execution_admission_snapshot_.store(nullptr, std::memory_order_release);
}

void Ros2ControlExecutionBackend::initialize_authored_state() {
  if (!uses_authored_backend_state()) {
    return;
  }

  std::vector<double> positions;
  if (is_commanded_sim_mode()) {
    positions = config_.initial_joint_positions_rad;
  } else {
    positions.reserve(config_.required_joints.size());
    for (const auto &joint_name : config_.required_joints) {
      const auto position =
          execution_state_sink_->group_joint_position(group_name_, joint_name);
      if (!position.has_value() || !std::isfinite(*position)) {
        positions.clear();
        break;
      }
      positions.push_back(*position);
    }
  }
  if (positions.size() != config_.required_joints.size()) {
    return;
  }

  // The authored initial vector is deployment authority, not inferred feedback.
  // Seed it before ROS callbacks start so factory composition cannot race a
  // measured diagnostic sample into the planning authority slot.
  (void)publish_snapshot(core::ExecutionState::kIdle, 0.0, positions);
}

void Ros2ControlExecutionBackend::publish_observed_robot_state(
    core::ObservedRobotStateSourceKind source,
    const std::vector<std::string> &joint_names,
    const std::vector<double> &positions, const std::vector<double> &velocities,
    std::int64_t source_timestamp_ns,
    std::int64_t received_timestamp_ns) noexcept {
  if (observed_state_publication_ == nullptr) {
    return;
  }
  auto *publication = observed_state_publication_;
  if (diagnostic_observed_state_publication_ != nullptr &&
      source != core::ObservedRobotStateSourceKind::kCommandedState &&
      source != core::ObservedRobotStateSourceKind::kPredictedState) {
    publication = diagnostic_observed_state_publication_.get();
  }
  if (joint_names.size() != positions.size() ||
      (!velocities.empty() && velocities.size() != joint_names.size())) {
    return;
  }
  const auto &transport_joints = transport_joint_names();
  std::vector<double> remapped_positions;
  std::vector<double> remapped_velocities;
  const std::vector<double> *ordered_positions = &positions;
  const std::vector<double> *ordered_velocities = &velocities;
  if (joint_names != transport_joints) {
    remapped_positions.reserve(transport_joints.size());
    if (!velocities.empty()) {
      remapped_velocities.reserve(transport_joints.size());
    }
    for (const auto &transport_joint : transport_joints) {
      const auto found =
          std::find(joint_names.begin(), joint_names.end(), transport_joint);
      if (found == joint_names.end() ||
          std::find(std::next(found), joint_names.end(), transport_joint) !=
              joint_names.end()) {
        return;
      }
      const auto index =
          static_cast<std::size_t>(std::distance(joint_names.begin(), found));
      remapped_positions.push_back(positions[index]);
      if (!velocities.empty()) {
        remapped_velocities.push_back(velocities[index]);
      }
    }
    ordered_positions = &remapped_positions;
    ordered_velocities = &remapped_velocities;
  }
  (void)publication->publish(source, config_.required_joints,
                             *ordered_positions, *ordered_velocities,
                             source_timestamp_ns, received_timestamp_ns);
}

bool Ros2ControlExecutionBackend::publish_authored_robot_state(
    std::span<const double> positions, std::span<const double> velocities,
    std::int64_t timestamp_ns,
    std::optional<std::uint64_t> expected_running_generation,
    bool invalidate_before_publish) noexcept {
  if (!uses_authored_backend_state() ||
      observed_state_publication_ == nullptr ||
      positions.size() != config_.required_joints.size()) {
    return false;
  }

  std::lock_guard<std::mutex> writer_lock(authored_state_publication_mutex_);
  if (expected_running_generation.has_value() &&
      running_generation_.load(std::memory_order_acquire) !=
          *expected_running_generation) {
    return false;
  }
  auto authority_epoch =
      authored_state_authority_epoch_.load(std::memory_order_relaxed);
  if (invalidate_before_publish) {
    // Non-expiring authored state must never remain admissible when a
    // lifecycle/terminal replacement cannot acquire a publication slot.
    if ((authority_epoch & 1u) == 0u) {
      ++authority_epoch;
      authored_state_authority_epoch_.store(authority_epoch,
                                            std::memory_order_release);
    }
  } else if ((authority_epoch & 1u) != 0u) {
    // A progress writer may update an already-valid active generation but may
    // never resurrect authority invalidated by a lifecycle/terminal boundary.
    return false;
  }

  const auto source = is_commanded_sim_mode()
                          ? core::ObservedRobotStateSourceKind::kCommandedState
                          : core::ObservedRobotStateSourceKind::kPredictedState;
  // Readers pin slots only while copying one bounded joint vector. A terminal
  // authority update gets a few bounded retries so transient slot pinning does
  // not leave the next plan on the previous command reference. The runtime
  // monitor performs exactly one try and never yields or waits on readers.
  const int attempt_count = invalidate_before_publish ? 8 : 1;
  for (int attempt = 0; attempt < attempt_count; ++attempt) {
    const auto result = observed_state_publication_->publish(
        source, config_.required_joints, positions, velocities, timestamp_ns,
        timestamp_ns);
    if (result.published) {
      if (expected_running_generation.has_value()) {
        authored_active_publications_total_.fetch_add(
            1, std::memory_order_relaxed);
        const auto previous = authored_active_last_publication_ns_.exchange(
            timestamp_ns, std::memory_order_acq_rel);
        if (previous > 0 && timestamp_ns >= previous) {
          atomic_max(authored_active_interarrival_max_ns_,
                     static_cast<std::uint64_t>(timestamp_ns - previous));
        }
      }
      if (invalidate_before_publish) {
        authored_state_authority_epoch_.store(authority_epoch + 1,
                                              std::memory_order_release);
      }
      return true;
    }
    if (std::string_view(result.reason) != "publication_slots_exhausted") {
      return false;
    }
    std::this_thread::yield();
  }
  return false;
}

void Ros2ControlExecutionBackend::initialize_ros() {
  context_ = std::make_shared<rclcpp::Context>();
  int argc = 0;
  char **argv = nullptr;
  context_->init(argc, argv);
  rclcpp::NodeOptions options;
  options.context(context_);
  node_ = std::make_shared<rclcpp::Node>(config_.node_name, options);

  action_callback_group_ = node_->create_callback_group(
      rclcpp::CallbackGroupType::MutuallyExclusive);
  state_callback_group_ = node_->create_callback_group(
      rclcpp::CallbackGroupType::MutuallyExclusive);
  binding_callback_group_ = node_->create_callback_group(
      rclcpp::CallbackGroupType::MutuallyExclusive);
  action_client_ = rclcpp_action::create_client<FollowJointTrajectory>(
      node_, config_.action_name, action_callback_group_);
  rclcpp::SubscriptionOptions state_subscription_options;
  state_subscription_options.callback_group = state_callback_group_;

  if (!config_.joint_state_topic.empty()) {
    joint_state_sub_ = node_->create_subscription<sensor_msgs::msg::JointState>(
        config_.joint_state_topic, rclcpp::SystemDefaultsQoS(),
        [this](const sensor_msgs::msg::JointState::SharedPtr msg) {
          on_joint_state(msg);
        },
        state_subscription_options);
  }
  if (!config_.controller_state_topic.empty()) {
    controller_state_sub_ = node_->create_subscription<
        control_msgs::msg::JointTrajectoryControllerState>(
        config_.controller_state_topic, rclcpp::SystemDefaultsQoS(),
        [this](
            const control_msgs::msg::JointTrajectoryControllerState::SharedPtr
                msg) { on_controller_state(msg); },
        state_subscription_options);
  }
  if (config_.controller_binding.has_value()) {
    rclcpp::SubscriptionOptions binding_subscription_options;
    binding_subscription_options.callback_group = binding_callback_group_;
    auto activity_qos = rclcpp::QoS(1).reliable().transient_local();
    controller_manager_activity_sub_ = node_->create_subscription<
        controller_manager_msgs::msg::ControllerManagerActivity>(
        config_.controller_binding->activity_topic, activity_qos,
        [this](const controller_manager_msgs::msg::ControllerManagerActivity::
                   SharedPtr msg) { on_controller_manager_activity(msg); },
        binding_subscription_options);
    list_controllers_client_ =
        node_->create_client<controller_manager_msgs::srv::ListControllers>(
            controller_manager_endpoint(
                config_.controller_binding->controller_manager,
                "list_controllers"),
            rclcpp::ServicesQoS(), binding_callback_group_);
    list_hardware_components_client_ = node_->create_client<
        controller_manager_msgs::srv::ListHardwareComponents>(
        controller_manager_endpoint(
            config_.controller_binding->controller_manager,
            "list_hardware_components"),
        rclcpp::ServicesQoS(), binding_callback_group_);
  }
  if (!config_.command_gate_topic.empty()) {
    auto qos = rclcpp::QoS(1).reliable().transient_local();
    command_gate_pub_ = node_->create_publisher<std_msgs::msg::Bool>(
        config_.command_gate_topic, qos);
    set_command_gate(false);
  }
#ifdef MANYFORGE_HAS_SPEED_SCALING_FACTOR_MSG
  if (config_.enable_jtc_speed_scaling_input) {
    std::string speed_scaling_topic = config_.speed_scaling_topic;
    if (speed_scaling_topic.empty()) {
      constexpr char suffix[] = "/follow_joint_trajectory";
      if (config_.action_name.size() >=
              std::char_traits<char>::length(suffix) &&
          config_.action_name.compare(
              config_.action_name.size() -
                  std::char_traits<char>::length(suffix),
              std::char_traits<char>::length(suffix), suffix) == 0) {
        speed_scaling_topic =
            config_.action_name.substr(
                0, config_.action_name.size() -
                       std::char_traits<char>::length(suffix)) +
            "/speed_scaling_input";
      } else {
        speed_scaling_topic = config_.action_name + "/speed_scaling_input";
      }
    }
    auto qos = rclcpp::QoS(1).reliable().transient_local();
    speed_scaling_pub_ =
        node_->create_publisher<control_msgs::msg::SpeedScalingFactor>(
            speed_scaling_topic, qos);
    publish_runtime_velocity_scale(
        runtime_velocity_scale_.load(std::memory_order_acquire));
  }
#endif

  rclcpp::ExecutorOptions executor_options;
  executor_options.context = context_;
  executor_ = std::make_shared<rclcpp::executors::MultiThreadedExecutor>(
      executor_options, config_.executor_thread_count);
  executor_->add_node(node_);
}

void Ros2ControlExecutionBackend::complete_ros_configuration() {
  std::lock_guard<std::mutex> lock(ros_configuration_mutex_);
  if (ros_configuration_complete_) {
    throw std::logic_error(
        "ros2_control backend configuration was already completed");
  }
  if (!executor_ || !node_) {
    throw std::logic_error(
        "ros2_control backend cannot start without initialized ROS resources");
  }
  auto spin_thread = std::thread([this]() { executor_->spin(); });
  spin_thread_ = std::move(spin_thread);
  ros_configuration_complete_ = true;
  start_controller_binding_monitor();
}

void Ros2ControlExecutionBackend::stop_ros() {
  stop_controller_binding_monitor();
  if (executor_) {
    executor_->cancel();
  }
  // The backend owns a private ROS context. Shut it down before joining so
  // teardown remains reliable when destruction races executor-thread startup:
  // a cancel issued just before spin() begins is not a durable stop condition,
  // whereas an invalid context is.
  if (context_) {
    context_->shutdown("manyforge ros2_control execution backend shutdown");
  }
  if (spin_thread_.joinable()) {
    spin_thread_.join();
  }
  if (executor_ && node_) {
    executor_->remove_node(node_);
  }
  node_.reset();
  executor_.reset();
  context_.reset();
}

void Ros2ControlExecutionBackend::start_controller_binding_monitor() {
  if (!config_.controller_binding.has_value()) {
    return;
  }
  if (controller_binding_monitor_thread_.joinable()) {
    throw std::logic_error("controller binding monitor was already started");
  }
  {
    std::lock_guard<std::mutex> lock(controller_binding_monitor_mutex_);
    controller_binding_refresh_requested_ = true;
  }
  controller_binding_monitor_thread_ =
      std::jthread([this](std::stop_token stop_token) {
        controller_binding_monitor_worker(stop_token);
      });
  controller_binding_monitor_cv_.notify_all();
}

void Ros2ControlExecutionBackend::stop_controller_binding_monitor() noexcept {
  if (!controller_binding_monitor_thread_.joinable()) {
    return;
  }
  controller_binding_monitor_thread_.request_stop();
  controller_binding_monitor_cv_.notify_all();
  controller_binding_monitor_thread_.join();
}

void Ros2ControlExecutionBackend::request_controller_binding_refresh() {
  {
    std::lock_guard<std::mutex> lock(controller_binding_monitor_mutex_);
    controller_binding_refresh_requested_ = true;
  }
  controller_binding_monitor_cv_.notify_all();
}

void Ros2ControlExecutionBackend::publish_controller_binding_snapshot(
    ControllerBindingSnapshot snapshot) {
  const auto now_ns = clock_.now();
  snapshot.generation = next_controller_binding_snapshot_generation_.fetch_add(
      1, std::memory_order_relaxed);
  if (snapshot.received_timestamp_ns == 0) {
    snapshot.received_timestamp_ns = now_ns;
  }
  if (snapshot.observed_timestamp_ns == 0) {
    snapshot.observed_timestamp_ns = now_ns;
  }
  if (snapshot.contract_generation_id.empty() &&
      config_.controller_binding.has_value()) {
    snapshot.contract_generation_id = config_.controller_binding->generation_id;
  }
  if (snapshot.activity_generation == 0) {
    snapshot.activity_generation =
        controller_binding_activity_generation_.load(std::memory_order_acquire);
  }
  auto immutable =
      std::make_shared<const ControllerBindingSnapshot>(std::move(snapshot));
  {
    std::lock_guard<std::mutex> lock(dispatch_admission_mutex_);
    controller_binding_snapshot_.store(std::move(immutable),
                                       std::memory_order_release);
  }
  execution_cv_.notify_all();
}

void Ros2ControlExecutionBackend::invalidate_controller_binding(
    std::string reason_code) {
  ControllerBindingSnapshot snapshot;
  snapshot.activity_generation =
      controller_binding_activity_generation_.load(std::memory_order_acquire);
  snapshot.received_timestamp_ns = clock_.now();
  snapshot.observed_timestamp_ns = snapshot.received_timestamp_ns;
  snapshot.fresh_until_ns =
      snapshot.received_timestamp_ns +
      (config_.controller_binding.has_value()
           ? config_.controller_binding->snapshot_max_age_ns
           : 1);
  snapshot.reason_codes.push_back(std::move(reason_code));
  publish_controller_binding_snapshot(std::move(snapshot));
}

void Ros2ControlExecutionBackend::on_controller_manager_activity(
    const controller_manager_msgs::msg::ControllerManagerActivity::SharedPtr) {
  controller_binding_activity_events_total_.fetch_add(
      1, std::memory_order_relaxed);
  controller_binding_activity_generation_.fetch_add(1,
                                                    std::memory_order_acq_rel);
  // Invalidate before any asynchronous service refresh. A trajectory may not
  // dispatch against the previous lifecycle/resource-claim generation.
  invalidate_controller_binding("controller_activity_changed");
  request_controller_binding_refresh();
}

void Ros2ControlExecutionBackend::controller_binding_monitor_worker(
    std::stop_token stop_token) {
  const auto &contract = *config_.controller_binding;
  const auto refresh_period = std::chrono::nanoseconds(
      std::max<std::int64_t>(1, contract.snapshot_max_age_ns / 2));
  const auto service_timeout =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::duration<double>(
              config_.controller_binding_refresh_timeout_s));

  while (!stop_token.stop_requested()) {
    {
      std::unique_lock<std::mutex> lock(controller_binding_monitor_mutex_);
      controller_binding_monitor_cv_.wait_for(
          lock, stop_token, refresh_period,
          [this]() { return controller_binding_refresh_requested_; });
      if (stop_token.stop_requested()) {
        return;
      }
      controller_binding_refresh_requested_ = false;
    }

    controller_binding_refresh_attempts_total_.fetch_add(
        1, std::memory_order_relaxed);
    const auto activity_generation =
        controller_binding_activity_generation_.load(std::memory_order_acquire);
    const auto fail_refresh = [this](std::string reason) {
      controller_binding_refresh_failures_total_.fetch_add(
          1, std::memory_order_relaxed);
      invalidate_controller_binding(std::move(reason));
    };

    if (!list_controllers_client_ || !list_hardware_components_client_) {
      fail_refresh("controller_binding_monitor_unavailable");
      continue;
    }
    if (!list_controllers_client_->wait_for_service(service_timeout)) {
      fail_refresh("controller_list_service_unavailable");
      continue;
    }
    if (!list_hardware_components_client_->wait_for_service(service_timeout)) {
      fail_refresh("hardware_list_service_unavailable");
      continue;
    }
    if (activity_generation == 0) {
      // The transient-local activity sample is the generation fence for the
      // two non-atomic controller-manager service reads. Do not admit an
      // initial snapshot until that fence has actually been observed.
      fail_refresh("controller_activity_unobserved");
      continue;
    }

    auto controllers_future = list_controllers_client_->async_send_request(
        std::make_shared<
            controller_manager_msgs::srv::ListControllers::Request>());
    if (controllers_future.wait_for(service_timeout) !=
        std::future_status::ready) {
      static_cast<void>(
          list_controllers_client_->remove_pending_request(controllers_future));
      fail_refresh("controller_list_service_timeout");
      continue;
    }
    auto hardware_future = list_hardware_components_client_->async_send_request(
        std::make_shared<
            controller_manager_msgs::srv::ListHardwareComponents::Request>());
    if (hardware_future.wait_for(service_timeout) !=
        std::future_status::ready) {
      static_cast<void>(
          list_hardware_components_client_->remove_pending_request(
              hardware_future));
      fail_refresh("hardware_list_service_timeout");
      continue;
    }

    if (controller_binding_activity_generation_.load(
            std::memory_order_acquire) != activity_generation) {
      request_controller_binding_refresh();
      continue;
    }

    const auto controllers = controllers_future.get();
    const auto hardware = hardware_future.get();
    ControllerBindingSnapshot snapshot;
    snapshot.activity_generation = activity_generation;
    snapshot.received_timestamp_ns = clock_.now();
    snapshot.observed_timestamp_ns = snapshot.received_timestamp_ns;
    snapshot.fresh_until_ns =
        snapshot.received_timestamp_ns + contract.snapshot_max_age_ns;

    const auto append_reason = [&snapshot](std::string reason) {
      if (std::find(snapshot.reason_codes.begin(), snapshot.reason_codes.end(),
                    reason) == snapshot.reason_codes.end()) {
        snapshot.reason_codes.push_back(std::move(reason));
      }
    };
    const auto exact_name_set = [](const std::vector<std::string> &lhs,
                                   const std::vector<std::string> &rhs) {
      if (lhs.size() != rhs.size()) {
        return false;
      }
      auto lhs_copy = lhs;
      auto rhs_copy = rhs;
      std::sort(lhs_copy.begin(), lhs_copy.end());
      std::sort(rhs_copy.begin(), rhs_copy.end());
      return std::adjacent_find(lhs_copy.begin(), lhs_copy.end()) ==
                 lhs_copy.end() &&
             std::adjacent_find(rhs_copy.begin(), rhs_copy.end()) ==
                 rhs_copy.end() &&
             lhs_copy == rhs_copy;
    };
    const auto intersects_expected =
        [&contract](const std::vector<std::string> &interfaces) {
          return std::any_of(
              interfaces.begin(), interfaces.end(),
              [&contract](const std::string &name) {
                return std::find(contract.claimed_command_interfaces.begin(),
                                 contract.claimed_command_interfaces.end(),
                                 name) !=
                       contract.claimed_command_interfaces.end();
              });
        };

    const controller_manager_msgs::msg::ControllerState *selected_controller =
        nullptr;
    std::size_t selected_controller_count = 0;
    snapshot.conflicting_claims_absent = true;
    for (const auto &controller : controllers->controller) {
      if (controller.name == contract.trajectory_controller_manager_name) {
        selected_controller = &controller;
        ++selected_controller_count;
      } else if (intersects_expected(controller.claimed_interfaces)) {
        snapshot.conflicting_claims_absent = false;
      }
    }
    if (selected_controller_count != 1 || selected_controller == nullptr) {
      append_reason(selected_controller_count == 0
                        ? "trajectory_controller_missing"
                        : "trajectory_controller_duplicated");
    } else {
      snapshot.controller_active = selected_controller->state == "active";
      snapshot.controller_type_matches =
          selected_controller->type == contract.trajectory_controller_type;
      snapshot.claimed_interfaces_match =
          exact_name_set(selected_controller->claimed_interfaces,
                         contract.claimed_command_interfaces);
      if (!snapshot.controller_active) {
        append_reason("trajectory_controller_inactive");
      }
      if (!snapshot.controller_type_matches) {
        append_reason("trajectory_controller_type_mismatch");
      }
      if (!snapshot.claimed_interfaces_match) {
        append_reason("trajectory_controller_claims_mismatch");
      }
    }
    if (!snapshot.conflicting_claims_absent) {
      append_reason("trajectory_controller_claim_conflict");
    }

    const controller_manager_msgs::msg::HardwareComponentState
        *selected_hardware = nullptr;
    std::size_t selected_hardware_count = 0;
    for (const auto &component : hardware->component) {
      if (component.name == contract.hardware_component_name) {
        selected_hardware = &component;
        ++selected_hardware_count;
      }
    }
    if (selected_hardware_count != 1 || selected_hardware == nullptr) {
      append_reason(selected_hardware_count == 0
                        ? "hardware_component_missing"
                        : "hardware_component_duplicated");
    } else {
      snapshot.hardware_active =
          selected_hardware->state.id ==
          lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE;
      if (!snapshot.hardware_active) {
        append_reason("hardware_component_inactive");
      }
      const auto command_interface_ready =
          [selected_hardware](const std::string &expected) {
            const auto found =
                std::find_if(selected_hardware->command_interfaces.begin(),
                             selected_hardware->command_interfaces.end(),
                             [&expected](const auto &interface) {
                               return interface.name == expected;
                             });
            return found != selected_hardware->command_interfaces.end() &&
                   found->is_available && found->is_claimed;
          };
      const auto state_interface_ready =
          [selected_hardware](const std::string &expected) {
            const auto found =
                std::find_if(selected_hardware->state_interfaces.begin(),
                             selected_hardware->state_interfaces.end(),
                             [&expected](const auto &interface) {
                               return interface.name == expected;
                             });
            return found != selected_hardware->state_interfaces.end() &&
                   found->is_available;
          };
      const bool hardware_commands_ready = std::all_of(
          contract.claimed_command_interfaces.begin(),
          contract.claimed_command_interfaces.end(), command_interface_ready);
      snapshot.state_interfaces_available = std::all_of(
          contract.required_state_interfaces.begin(),
          contract.required_state_interfaces.end(), state_interface_ready);
      if (!hardware_commands_ready) {
        snapshot.claimed_interfaces_match = false;
        append_reason("hardware_command_interfaces_unavailable");
      }
      if (!snapshot.state_interfaces_available) {
        append_reason("hardware_state_interfaces_unavailable");
      }
    }

    snapshot.valid =
        snapshot.controller_active && snapshot.hardware_active &&
        snapshot.controller_type_matches && snapshot.claimed_interfaces_match &&
        snapshot.state_interfaces_available &&
        snapshot.conflicting_claims_absent && snapshot.reason_codes.empty();
    if (controller_binding_activity_generation_.load(
            std::memory_order_acquire) != activity_generation) {
      request_controller_binding_refresh();
      continue;
    }
    publish_controller_binding_snapshot(std::move(snapshot));
    controller_binding_refresh_successes_total_.fetch_add(
        1, std::memory_order_relaxed);
  }
}

void Ros2ControlExecutionBackend::on_joint_state(
    const sensor_msgs::msg::JointState::SharedPtr msg) {
  state_callbacks_total_.fetch_add(1, std::memory_order_relaxed);
  const auto received_timestamp_ns = clock_.now();
  publish_observed_robot_state(
      core::ObservedRobotStateSourceKind::kJointState, msg->name, msg->position,
      msg->velocity, ros_stamp_ns(msg->header.stamp), received_timestamp_ns);
  JointSample sample;
  sample.joint_names = msg->name;
  sample.positions = msg->position;
  sample.velocities = msg->velocity;
  sample.received_at = std::chrono::steady_clock::now();
  sample.received_timestamp_ns = received_timestamp_ns;
  sample.source = "joint_state";
  sample.topic = config_.joint_state_topic;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    latest_joint_state_ = std::move(sample);
  }
  state_cv_.notify_all();
  sync_idle_scene_from_latest_sample();
}

std::uint64_t Ros2ControlExecutionBackend::
    begin_jtc_cancel_reference_authority_locked() noexcept {
  if (!is_commanded_sim_mode() || jtc_cancel_reference_authority_token_.load(
                                      std::memory_order_acquire) != 0) {
    return 0;
  }
  auto token = next_jtc_cancel_reference_authority_token_++;
  if (token == 0) {
    token = next_jtc_cancel_reference_authority_token_++;
  }
  jtc_cancel_reference_authority_token_.store(token, std::memory_order_release);
  return token;
}

void Ros2ControlExecutionBackend::end_jtc_cancel_reference_authority(
    std::uint64_t token) noexcept {
  if (token == 0) {
    return;
  }
  // The callback holds execution_mutex_ through the bounded publication, so
  // once this returns no callback from this delegation can publish later.
  std::lock_guard<std::mutex> lock(execution_mutex_);
  if (jtc_cancel_reference_authority_token_.load(std::memory_order_acquire) ==
      token) {
    jtc_cancel_reference_authority_token_.store(0, std::memory_order_release);
  }
}

bool Ros2ControlExecutionBackend::publish_jtc_cancel_reference(
    const std::vector<std::string> &joint_names,
    const std::vector<double> &positions, const std::vector<double> &velocities,
    std::int64_t received_timestamp_ns, std::uint64_t expected_token) noexcept {
  const auto reject = [this]() {
    jtc_cancel_reference_rejections_total_.fetch_add(1,
                                                     std::memory_order_relaxed);
    return false;
  };
  if (expected_token == 0 || jtc_cancel_reference_authority_token_.load(
                                 std::memory_order_acquire) != expected_token) {
    return reject();
  }
  if (joint_names.size() != positions.size() ||
      (!velocities.empty() && velocities.size() != joint_names.size())) {
    return reject();
  }

  std::vector<double> remapped_positions;
  std::vector<double> remapped_velocities;
  std::span<const double> ordered_positions = positions;
  std::span<const double> ordered_velocities = velocities;
  const auto &transport_joints = transport_joint_names();
  if (joint_names == transport_joints) {
    if (!std::all_of(positions.begin(), positions.end(),
                     [](double value) { return std::isfinite(value); }) ||
        !std::all_of(velocities.begin(), velocities.end(),
                     [](double value) { return std::isfinite(value); })) {
      return reject();
    }
  } else {
    // JTC normally publishes canonical order, so the steady stop path above
    // adds no allocations. Keep a correct bounded remap for adapters whose
    // controller order differs from the deployment order.
    remapped_positions.reserve(transport_joints.size());
    if (!velocities.empty()) {
      remapped_velocities.reserve(transport_joints.size());
    }
    for (const auto &required_joint : transport_joints) {
      const auto first =
          std::find(joint_names.begin(), joint_names.end(), required_joint);
      if (first == joint_names.end() ||
          std::find(std::next(first), joint_names.end(), required_joint) !=
              joint_names.end()) {
        return reject();
      }
      const auto index =
          static_cast<std::size_t>(std::distance(joint_names.begin(), first));
      if (!std::isfinite(positions[index])) {
        return reject();
      }
      remapped_positions.push_back(positions[index]);
      if (!velocities.empty()) {
        if (!std::isfinite(velocities[index])) {
          return reject();
        }
        remapped_velocities.push_back(velocities[index]);
      }
    }
    ordered_positions = remapped_positions;
    ordered_velocities = remapped_velocities;
  }

  // Token revalidation and publication are one execution-owned operation.
  // This closes both late-callback and old-stop/new-stop ABA windows without
  // making measured feedback authoritative in commanded_sim.
  std::lock_guard<std::mutex> lock(execution_mutex_);
  if (jtc_cancel_reference_authority_token_.load(std::memory_order_acquire) !=
      expected_token) {
    return reject();
  }
  const bool published = publish_authored_robot_state(
      ordered_positions, ordered_velocities, received_timestamp_ns,
      std::nullopt, /*invalidate_before_publish=*/false);
  if (!published) {
    return reject();
  }
  jtc_cancel_reference_publications_total_.fetch_add(1,
                                                     std::memory_order_relaxed);
  return true;
}

void Ros2ControlExecutionBackend::on_controller_state(
    const control_msgs::msg::JointTrajectoryControllerState::SharedPtr msg) {
  state_callbacks_total_.fetch_add(1, std::memory_order_relaxed);
  const auto received_timestamp_ns = clock_.now();
  publish_observed_robot_state(
      core::ObservedRobotStateSourceKind::kControllerState, msg->joint_names,
      msg->feedback.positions, msg->feedback.velocities,
      ros_stamp_ns(msg->header.stamp), received_timestamp_ns);
  const auto jtc_cancel_reference_token =
      jtc_cancel_reference_authority_token_.load(std::memory_order_acquire);
  if (jtc_cancel_reference_token != 0) {
    (void)publish_jtc_cancel_reference(
        msg->joint_names, msg->reference.positions, msg->reference.velocities,
        received_timestamp_ns, jtc_cancel_reference_token);
  }
  JointSample sample;
  sample.joint_names = msg->joint_names;
  sample.positions = msg->feedback.positions;
  sample.velocities = msg->feedback.velocities;
  sample.received_at = std::chrono::steady_clock::now();
  sample.received_timestamp_ns = received_timestamp_ns;
  sample.source = "controller_state";
  sample.topic = config_.controller_state_topic;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    latest_controller_state_ = std::move(sample);
  }
  state_cv_.notify_all();
  sync_idle_scene_from_latest_sample();
}

void Ros2ControlExecutionBackend::on_action_feedback(
    GoalHandleFJT::SharedPtr goal_handle,
    const std::shared_ptr<const FollowJointTrajectory::Feedback> feedback) {
  action_feedback_callbacks_total_.fetch_add(1, std::memory_order_relaxed);
  const auto now = std::chrono::steady_clock::now();
  double elapsed_s = 0.0;
  bool active_goal_feedback = false;
  bool managed_fjt_terminal_feedback = false;
  std::uint64_t feedback_generation = 0;
  std::optional<rclcpp_action::GoalUUID> feedback_goal_id;
  {
    std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
    managed_fjt_terminal_feedback =
        goal_handle && managed_fjt_terminal_goal_id_.has_value() &&
        goal_handle->get_goal_id() == *managed_fjt_terminal_goal_id_;
  }
  {
    std::lock_guard<std::mutex> lock(execution_mutex_);
    if (resync_required_) {
      return;
    }
    active_goal_feedback = goal_handle && active_goal_id_.has_value() &&
                           goal_handle->get_goal_id() == *active_goal_id_;
    if (!active_goal_feedback && !managed_fjt_terminal_feedback) {
      return;
    }
    if (terminal_ && !managed_fjt_terminal_feedback) {
      return;
    }
    if (active_goal_feedback) {
      feedback_generation = running_generation_.load(std::memory_order_acquire);
      feedback_goal_id = goal_handle->get_goal_id();
    }
    if (active_goal_feedback && running_ && !first_feedback_seen_) {
      record_command_progress_latency_ns(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              now - active_goal_accepted_at_)
              .count());
      first_feedback_seen_ = true;
      execution_started_at_ = now;
      tracking_error_since_.reset();
      // Audit 6C: the monitor's first-feedback wait keys on this flag.
      execution_cv_.notify_all();
    }
    elapsed_s = seconds_since(execution_started_at_, now);
  }
  if (!active_goal_feedback) {
    // Managed replacement-goal feedback is useful for path-deviation
    // evidence, but it is not the physical-state authority of the original
    // execution and must never replace the active-goal cache.
    if (managed_fjt_terminal_feedback &&
        controlled_stop_path_deviation_tracking_active_.load(
            std::memory_order_acquire)) {
      record_controlled_stop_path_deviation_feedback(
          feedback->joint_names, feedback->actual.positions);
    }
    return;
  }

  const auto &expected_positions = feedback->desired.positions;
  const auto received_timestamp_ns = clock_.now();
  {
    // Revalidate immediately before publication/cache insertion. The first
    // check above may race Reset, replacement, or terminalization.
    std::lock_guard<std::mutex> execution_lock(execution_mutex_);
    if (!running_ || terminal_ || resync_required_ ||
        running_generation_.load(std::memory_order_acquire) !=
            feedback_generation ||
        !active_goal_id_.has_value() || !feedback_goal_id.has_value() ||
        *active_goal_id_ != *feedback_goal_id) {
      return;
    }
    publish_observed_robot_state(
        core::ObservedRobotStateSourceKind::kActionFeedback,
        feedback->joint_names, feedback->actual.positions,
        feedback->actual.velocities, ros_stamp_ns(feedback->header.stamp),
        received_timestamp_ns);
    std::lock_guard<std::mutex> state_lock(state_mutex_);
    if (!latest_action_feedback_.has_value()) {
      latest_action_feedback_.emplace();
    }
    auto &sample = *latest_action_feedback_;
    assign_joint_sample(sample, feedback->joint_names,
                        feedback->actual.positions, feedback->actual.velocities,
                        now, received_timestamp_ns, "action_feedback",
                        config_.action_name);
    sample.execution_generation = feedback_generation;
    sample.goal_id = feedback_goal_id;
    const auto &observed_positions = sample.positions;
    const auto &observed_velocities = sample.velocities;
    if (active_goal_feedback && !feedback->error.positions.empty()) {
      if (!latest_action_feedback_error_.has_value()) {
        latest_action_feedback_error_.emplace();
      }
      auto &error_sample = *latest_action_feedback_error_;
      assign_joint_sample(error_sample, feedback->joint_names,
                          feedback->error.positions, feedback->error.velocities,
                          now, received_timestamp_ns, "action_feedback_error",
                          config_.action_name);
      error_sample.execution_generation = feedback_generation;
      error_sample.goal_id = feedback_goal_id;
      update_tracking_error_peak_locked(
          error_sample, elapsed_s,
          observed_positions.empty() ? nullptr : &observed_positions,
          expected_positions.empty() ? nullptr : &expected_positions,
          observed_velocities.empty() ? nullptr : &observed_velocities);
    }
  }
  state_cv_.notify_all();
}

void Ros2ControlExecutionBackend::on_action_result(
    const GoalHandleFJT::WrappedResult &result,
    std::uint64_t execution_generation) {
  const auto now = std::chrono::steady_clock::now();
  action_result_callbacks_total_.fetch_add(1, std::memory_order_relaxed);
  const bool canceled = result.code == rclcpp_action::ResultCode::CANCELED;
  {
    std::lock_guard<std::mutex> lock(execution_mutex_);
    if (terminal_ || resync_required_ || !running_ ||
        running_generation_.load(std::memory_order_acquire) !=
            execution_generation) {
      return;
    }
    if (!active_goal_id_.has_value() || result.goal_id != *active_goal_id_) {
      return;
    }
    result_received_ = true;
    record_action_result_latency_ns(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            now - active_goal_accepted_at_)
            .count());
    // Persist the raw result code so a parallel
    // `execute_controlled_stop(kHardDeceleration)` can verify the
    // cancel was actually honored. The stop_requested branch below
    // collapses SUCCEEDED + CANCELED into "stopped"; the raw code is
    // the only place the distinction survives.
    last_action_result_code_ = result.code;
    if (controlled_stop_preempt_in_progress_ &&
        (stop_requested_ || pause_requested_ || canceled)) {
      // A managed controlled stop preempts the active JTC goal with a second
      // FollowJointTrajectory goal. The terminal result for the OLD goal is
      // only evidence that preemption happened; the managed replacement goal
      // owns terminal state and scene publishing for the stop window.
      active_action_feedback_authority_generation_.store(
          0, std::memory_order_release);
      active_goal_handle_.reset();
      active_goal_id_.reset();
      execution_cv_.notify_all();
      return;
    }
  }

  if (canceled) {
    commit_action_result_terminal(result.goal_id, execution_generation,
                                  canceled, std::nullopt, now);
    return;
  }

  const bool controller_success =
      result.result &&
      result.result->error_code == FollowJointTrajectory::Result::SUCCESSFUL;
  if (result.code != rclcpp_action::ResultCode::SUCCEEDED ||
      !controller_success) {
    const std::string controller_error = result.result
                                             ? result.result->error_string
                                             : "controller returned no result";
    const std::string feedback_error = latest_feedback_error_summary();
    std::string detail = controller_error;
    if (!feedback_error.empty()) {
      detail = detail.empty() ? feedback_error : detail + "; " + feedback_error;
    }
    commit_action_result_terminal(
        result.goal_id, execution_generation, false,
        make_ros2_control_fault_error(
            core::ErrorCode::kExecutionAborted,
            detail.empty() ? "FollowJointTrajectory goal failed"
                           : "FollowJointTrajectory goal failed: " + detail,
            ros2_control_fault_reason::kControllerResultFailed,
            {{"action_result_code",
              std::to_string(static_cast<int>(result.code))},
             {"controller_result_code",
              result.result ? std::to_string(result.result->error_code)
                            : "unavailable"},
             {"controller_error", controller_error},
             {"action_name", config_.action_name}}),
        now);
    return;
  }

  bool missing_completion_trajectory = false;
  {
    std::lock_guard<std::mutex> lock(execution_mutex_);
    if (terminal_ || !running_ ||
        running_generation_.load(std::memory_order_acquire) !=
            execution_generation) {
      return;
    }
    if (!active_goal_id_.has_value() || *active_goal_id_ != result.goal_id) {
      return;
    }
    if (!active_trajectory_) {
      missing_completion_trajectory = true;
    } else {
      completion_gate_request_ = CompletionGateRequest{
          execution_generation,
          result.goal_id,
          now,
          active_trajectory_,
      };
    }
  }
  if (missing_completion_trajectory) {
    commit_action_result_terminal(
        result.goal_id, execution_generation, false,
        make_ros2_control_fault_error(
            core::ErrorCode::kInvariantViolation,
            "Controller reported success without an active trajectory for "
            "measured final-state verification",
            ros2_control_fault_reason::kTrackingSampleInvalid,
            {{"phase", "terminal_result_state_gate"},
             {"action_name", config_.action_name}}),
        now);
    return;
  }
  // The ROS action callback must stay non-blocking. The existing dedicated
  // NRT monitor thread owns the bounded post-result measured-state gate.
  execution_cv_.notify_all();
}

std::optional<Ros2ControlExecutionBackend::JointSample>
Ros2ControlExecutionBackend::select_latest_sample_locked(
    std::chrono::steady_clock::time_point now) const {
  const JointSample *sample = select_latest_sample_ptr_locked(now);
  if (sample != nullptr) {
    return *sample;
  }
  return std::nullopt;
}

const Ros2ControlExecutionBackend::JointSample *
Ros2ControlExecutionBackend::select_latest_sample_ptr_locked(
    std::chrono::steady_clock::time_point now) const {
  for (const auto &source : config_.observed_state_source_precedence) {
    if (source == "action_feedback" && latest_action_feedback_.has_value() &&
        joint_sample_is_fresh_and_valid(*latest_action_feedback_, now) &&
        action_sample_matches_active_execution_locked(
            *latest_action_feedback_)) {
      return &*latest_action_feedback_;
    }
    if (source == "controller_state" && latest_controller_state_.has_value() &&
        joint_sample_is_fresh_and_valid(*latest_controller_state_, now)) {
      return &*latest_controller_state_;
    }
    if (source == "joint_state" && latest_joint_state_.has_value() &&
        joint_sample_is_fresh_and_valid(*latest_joint_state_, now)) {
      return &*latest_joint_state_;
    }
  }
  return nullptr;
}

const Ros2ControlExecutionBackend::JointSample *
Ros2ControlExecutionBackend::select_latest_passive_sample_ptr_locked(
    std::chrono::steady_clock::time_point now) const {
  for (const auto &source : config_.observed_state_source_precedence) {
    // Once a goal is terminal, action feedback may be late or correlated with
    // the retired goal. Passive physical state comes only from the configured
    // controller/joint streams, in their deployment-declared order.
    if (source == "controller_state" && latest_controller_state_.has_value() &&
        joint_sample_is_fresh_and_valid(*latest_controller_state_, now)) {
      return &*latest_controller_state_;
    }
    if (source == "joint_state" && latest_joint_state_.has_value() &&
        joint_sample_is_fresh_and_valid(*latest_joint_state_, now)) {
      return &*latest_joint_state_;
    }
  }
  return nullptr;
}

std::optional<Ros2ControlExecutionBackend::JointSample>
Ros2ControlExecutionBackend::select_latest_passive_sample_after_locked(
    std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::time_point not_before) const {
  for (const auto &source : config_.observed_state_source_precedence) {
    if (source == "controller_state" && latest_controller_state_.has_value() &&
        joint_sample_is_fresh_and_valid(*latest_controller_state_, now,
                                        not_before)) {
      return latest_controller_state_;
    }
    if (source == "joint_state" && latest_joint_state_.has_value() &&
        joint_sample_is_fresh_and_valid(*latest_joint_state_, now,
                                        not_before)) {
      return latest_joint_state_;
    }
  }
  return std::nullopt;
}

bool Ros2ControlExecutionBackend::joint_sample_is_complete_and_finite(
    const JointSample &sample) const noexcept {
  if (sample.received_timestamp_ns < 0 || sample.joint_names.empty() ||
      sample.joint_names.size() != sample.positions.size() ||
      (!sample.velocities.empty() &&
       sample.velocities.size() != sample.joint_names.size())) {
    return false;
  }
  for (std::size_t index = 0; index < sample.joint_names.size(); ++index) {
    if (sample.joint_names[index].empty() ||
        !std::isfinite(sample.positions[index]) ||
        (!sample.velocities.empty() &&
         !std::isfinite(sample.velocities[index]))) {
      return false;
    }
    if (std::find(sample.joint_names.begin(),
                  sample.joint_names.begin() +
                      static_cast<std::ptrdiff_t>(index),
                  sample.joint_names[index]) !=
        sample.joint_names.begin() + static_cast<std::ptrdiff_t>(index)) {
      return false;
    }
  }
  for (const auto &required_joint : transport_joint_names()) {
    if (std::count(sample.joint_names.begin(), sample.joint_names.end(),
                   required_joint) != 1) {
      return false;
    }
  }
  return true;
}

bool Ros2ControlExecutionBackend::joint_sample_is_fresh_and_valid(
    const JointSample &sample, std::chrono::steady_clock::time_point now,
    std::optional<std::chrono::steady_clock::time_point> not_before)
    const noexcept {
  if (!joint_sample_is_complete_and_finite(sample) ||
      sample.received_at > now ||
      (not_before.has_value() && sample.received_at < *not_before)) {
    return false;
  }
  return seconds_since(sample.received_at, now) <=
         config_.joint_state_max_age_s;
}

bool Ros2ControlExecutionBackend::action_sample_matches_active_execution_locked(
    const JointSample &sample) const noexcept {
  // Exact goal equality is enforced immediately before the sample enters the
  // cache. The monotonically changing execution generation is then the
  // lock-free retirement fence used by state-only readers; they must not read
  // active_goal_id_, which is protected by execution_mutex_.
  return running_flag_.load(std::memory_order_acquire) &&
         sample.execution_generation.has_value() &&
         *sample.execution_generation ==
             running_generation_.load(std::memory_order_acquire) &&
         active_action_feedback_authority_generation_.load(
             std::memory_order_acquire) == *sample.execution_generation &&
         sample.goal_id.has_value();
}

void Ros2ControlExecutionBackend::sync_idle_scene_from_latest_sample() {
  if (uses_authored_backend_state()) {
    return;
  }
  if (running_flag_.load(std::memory_order_acquire)) {
    return;
  }

  std::vector<double> positions;
  std::vector<double> velocities;
  std::int64_t source_timestamp_ns = 0;
  const auto now = std::chrono::steady_clock::now();
  const auto generation = running_generation_.load(std::memory_order_acquire);
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    const JointSample *sample = select_latest_passive_sample_ptr_locked(now);
    if (sample == nullptr) {
      return;
    }
    if (idle_scene_sync_generation_ != generation) {
      idle_scene_sync_generation_ = generation;
      last_idle_scene_sample_received_at_.reset();
      last_idle_scene_projection_at_ = {};
    }
    if (last_idle_scene_sample_received_at_.has_value() &&
        *last_idle_scene_sample_received_at_ == sample->received_at) {
      return;
    }
    if (last_idle_scene_projection_at_ !=
            std::chrono::steady_clock::time_point{} &&
        now - last_idle_scene_projection_at_ < kIdleSceneProjectionMinPeriod) {
      return;
    }
    last_idle_scene_sample_received_at_ = sample->received_at;
    last_idle_scene_projection_at_ = now;
    try {
      positions = positions_for_joints(*sample, transport_joint_names());
      velocities = velocities_for_joints(*sample, transport_joint_names());
      source_timestamp_ns = sample->received_timestamp_ns;
    } catch (const std::exception &) {
      return;
    }
  }

  // Serialize the snapshot and scene submissions with execution start. A
  // generation check after queuing the scene request is too late: an old idle
  // sample could otherwise be queued behind a complete newer execution.
  std::lock_guard<std::mutex> execution_lock(execution_mutex_);
  if (running_ ||
      running_generation_.load(std::memory_order_acquire) != generation) {
    return;
  }
  if (!publish_passive_observed_snapshot(positions, velocities,
                                         source_timestamp_ns, generation)) {
    return;
  }
  (void)schedule_active_scene_projection(positions, source_timestamp_ns);
}

bool Ros2ControlExecutionBackend::is_commanded_sim_mode() const {
  return config_.execution_state_source_mode == "commanded_sim";
}

bool Ros2ControlExecutionBackend::uses_authored_backend_state() const {
  return config_.execution_state_source_mode == "authored_preview" ||
         is_commanded_sim_mode();
}

void Ros2ControlExecutionBackend::set_command_gate(bool enabled) {
  if (!command_gate_pub_) {
    return;
  }
  // Tracked so goal-rejection diagnostics can state when the gate is the
  // cause (a closed gate makes the bridge reject every goal, which would
  // otherwise surface as an unexplained rejection).
  command_gate_enabled_.store(enabled, std::memory_order_release);
  std_msgs::msg::Bool msg;
  msg.data = enabled;
  command_gate_pub_->publish(msg);
}

bool Ros2ControlExecutionBackend::request_command_gate(bool enabled) {
  if (!command_gate_pub_) {
    return false;
  }
  set_command_gate(enabled);
  return true;
}

void Ros2ControlExecutionBackend::publish_runtime_velocity_scale(double scale) {
#ifdef MANYFORGE_HAS_SPEED_SCALING_FACTOR_MSG
  if (!speed_scaling_pub_) {
    return;
  }
  control_msgs::msg::SpeedScalingFactor msg;
  msg.factor = std::max(0.0, std::min(1.0, scale));
  speed_scaling_pub_->publish(msg);
#else
  (void)scale;
#endif
}

void Ros2ControlExecutionBackend::begin_controlled_stop_runtime_scale_freeze(
    bool assert_unit_scale) {
  {
    std::lock_guard<std::mutex> lock(runtime_velocity_scale_mutex_);
    if (controlled_stop_runtime_scale_freeze_depth_ == 0) {
      controlled_stop_runtime_scale_restore_ =
          runtime_velocity_scale_.load(std::memory_order_acquire);
      deferred_runtime_velocity_scale_.reset();
      controlled_stop_unit_scale_asserted_ = false;
    }
    ++controlled_stop_runtime_scale_freeze_depth_;
  }
  if (assert_unit_scale) {
    assert_controlled_stop_unit_runtime_scale();
  }
}

void Ros2ControlExecutionBackend::assert_controlled_stop_unit_runtime_scale() {
  bool publish_unit_scale = false;
  {
    std::lock_guard<std::mutex> lock(runtime_velocity_scale_mutex_);
    if (controlled_stop_runtime_scale_freeze_depth_ <= 0 ||
        controlled_stop_unit_scale_asserted_) {
      return;
    }
    controlled_stop_unit_scale_asserted_ = true;
    publish_unit_scale = true;
  }
  if (publish_unit_scale && config_.enable_jtc_speed_scaling_input) {
    publish_runtime_velocity_scale(1.0);
  }
}

bool Ros2ControlExecutionBackend::end_controlled_stop_runtime_scale_freeze() {
  std::optional<double> scale_to_publish;
  bool applied_deferred_scale = false;
  {
    std::lock_guard<std::mutex> lock(runtime_velocity_scale_mutex_);
    if (controlled_stop_runtime_scale_freeze_depth_ <= 0) {
      return false;
    }
    --controlled_stop_runtime_scale_freeze_depth_;
    if (controlled_stop_runtime_scale_freeze_depth_ == 0) {
      if (deferred_runtime_velocity_scale_.has_value()) {
        runtime_velocity_scale_.store(*deferred_runtime_velocity_scale_,
                                      std::memory_order_release);
        scale_to_publish = *deferred_runtime_velocity_scale_;
        deferred_runtime_velocity_scale_.reset();
        applied_deferred_scale = true;
      } else if (controlled_stop_unit_scale_asserted_ &&
                 controlled_stop_runtime_scale_restore_.has_value()) {
        scale_to_publish = *controlled_stop_runtime_scale_restore_;
      }
      controlled_stop_runtime_scale_restore_.reset();
      controlled_stop_unit_scale_asserted_ = false;
    }
  }
  if (scale_to_publish.has_value() && config_.enable_jtc_speed_scaling_input) {
    publish_runtime_velocity_scale(*scale_to_publish);
  }
  execution_cv_.notify_all();
  managed_fjt_cv_.notify_all();
  return applied_deferred_scale;
}

core::Result<void>
Ros2ControlExecutionBackend::command_sim_initial_state(std::string phase) {
  if (!is_commanded_sim_mode()) {
    return core::Result<void>::Ok();
  }
  if (config_.initial_joint_positions_rad.size() !=
      config_.required_joints.size()) {
    return core::Result<void>::Err(core::ErrorDTO{
        core::kErrorInvalidRequest,
        "commanded_sim initial_joint_positions_rad is incomplete",
        {{"phase", std::move(phase)},
         {"state_source_mode", config_.execution_state_source_mode}},
    });
  }
  if (!wait_for_action_server_until(action_client_,
                                    config_.wait_for_action_server_timeout_s)) {
    set_command_gate(false);
    return core::Result<void>::Err(core::ErrorDTO{
        core::kErrorExecutionAborted,
        "FollowJointTrajectory action server unavailable for commanded_sim "
        "reset",
        {{"phase", std::move(phase)},
         {"action_name", config_.action_name},
         {"wait_for_action_server_timeout_s",
          std::to_string(config_.wait_for_action_server_timeout_s)}},
    });
  }

  FollowJointTrajectory::Goal goal;
  goal.trajectory.joint_names = transport_joint_names();
  disable_fjt_controller_tolerances(goal, transport_joint_names());
  trajectory_msgs::msg::JointTrajectoryPoint home_point;
  home_point.positions = config_.initial_joint_positions_rad;
  home_point.time_from_start =
      duration_from_seconds(config_.sim_reset_motion_duration_s);
  goal.trajectory.points.push_back(std::move(home_point));

  // The commanded-sim profile is backend-authoritative: suppress any stale
  // topic-based command stream until this reset/home goal has been accepted.
  set_command_gate(false);
  auto goal_handle_future = action_client_->async_send_goal(goal);
  if (goal_handle_future.wait_for(std::chrono::duration<double>(
          config_.wait_for_action_server_timeout_s)) !=
      std::future_status::ready) {
    set_command_gate(false);
    cancel_all_fjt_goals();
    return core::Result<void>::Err(core::ErrorDTO{
        core::kErrorExecutionTimeout,
        "Timed out waiting for commanded_sim reset goal response",
        {{"phase", std::move(phase)}, {"action_name", config_.action_name}},
    });
  }
  GoalHandleFJT::SharedPtr goal_handle;
  try {
    goal_handle = goal_handle_future.get();
  } catch (const std::exception &exc) {
    set_command_gate(false);
    return core::Result<void>::Err(core::ErrorDTO{
        core::kErrorExecutionAborted,
        std::string("Failed to dispatch commanded_sim reset goal: ") +
            exc.what(),
        {{"phase", std::move(phase)}, {"action_name", config_.action_name}},
    });
  }
  if (!goal_handle) {
    set_command_gate(false);
    return core::Result<void>::Err(core::ErrorDTO{
        core::kErrorExecutionAborted,
        "commanded_sim reset goal was rejected",
        {{"phase", std::move(phase)}, {"action_name", config_.action_name}},
    });
  }

  set_command_gate(true);
  const auto result_future = action_client_->async_get_result(goal_handle);
  const double timeout_s = config_.sim_reset_motion_duration_s +
                           config_.wait_for_action_server_timeout_s +
                           config_.stop_completion_timeout_s;
  if (result_future.wait_for(std::chrono::duration<double>(timeout_s)) !=
      std::future_status::ready) {
    (void)action_client_->async_cancel_goal(goal_handle);
    set_command_gate(false);
    return core::Result<void>::Err(core::ErrorDTO{
        core::kErrorExecutionTimeout,
        "Timed out waiting for commanded_sim reset goal result",
        {{"phase", std::move(phase)},
         {"action_name", config_.action_name},
         {"timeout_s", std::to_string(timeout_s)}},
    });
  }
  const auto result = result_future.get();
  const bool success =
      result.code == rclcpp_action::ResultCode::SUCCEEDED && result.result &&
      result.result->error_code == FollowJointTrajectory::Result::SUCCESSFUL;
  if (!success) {
    std::string detail = result.result ? result.result->error_string
                                       : "controller returned no result";
    if (detail.empty()) {
      detail = "controller returned non-success result";
    }
    set_command_gate(false);
    return core::Result<void>::Err(core::ErrorDTO{
        core::kErrorExecutionAborted,
        "commanded_sim reset goal failed: " + detail,
        {{"phase", std::move(phase)}, {"action_name", config_.action_name}},
    });
  }

  try {
    update_scene_positions(config_.initial_joint_positions_rad);
  } catch (const std::exception &exc) {
    set_command_gate(false);
    return core::Result<void>::Err(core::ErrorDTO{
        core::kErrorExecutionAborted,
        std::string(
            "commanded_sim reset could not publish the authoritative ") +
            "scene state: " + exc.what(),
        {{"phase", std::move(phase)},
         {"action_name", config_.action_name},
         {"reason", "reset_scene_projection_failed"}},
    });
  }
  publish_snapshot(core::ExecutionState::kIdle, 0.0,
                   config_.initial_joint_positions_rad);
  return core::Result<void>::Ok();
}

core::Result<void>
Ros2ControlExecutionBackend::sync_observed_state_before_planning() {
  if (config_.controller_binding.has_value() ||
      config_.require_execution_admission_snapshot) {
    std::lock_guard<std::mutex> lock(dispatch_admission_mutex_);
    planning_environment_authority_.reset();
  }
  auto resync_result = resync_after_intervention_if_needed("pre_plan_resync");
  if (!resync_result.ok()) {
    return resync_result;
  }

  if (uses_authored_backend_state()) {
    return capture_planning_environment_authority();
  }
  if (!config_.require_fresh_state_before_planning) {
    return capture_planning_environment_authority();
  }

  auto sample = wait_for_fresh_passive_sample(config_.wait_for_state_timeout_s);
  if (!sample.has_value()) {
    return core::Result<void>::Err(core::ErrorDTO{
        core::kErrorStartStateMismatch,
        "No fresh complete observed robot state before planning",
        {
            {"state_source_mode", config_.execution_state_source_mode},
            {"max_age_s", std::to_string(config_.joint_state_max_age_s)},
            {"timeout_s", std::to_string(config_.wait_for_state_timeout_s)},
        },
    });
  }

  std::vector<double> positions;
  std::vector<double> velocities;
  try {
    positions = positions_for_joints(*sample, transport_joint_names());
    velocities = velocities_for_joints(*sample, transport_joint_names());
  } catch (const std::exception &exc) {
    return core::Result<void>::Err(core::ErrorDTO{
        core::kErrorStartStateMismatch,
        std::string("Observed robot state is incomplete before planning: ") +
            exc.what(),
        {
            {"state_source_mode", config_.execution_state_source_mode},
            {"sample_source", sample->source},
            {"sample_topic", sample->topic},
        },
    });
  }

  try {
    update_scene_positions(positions);
  } catch (const std::exception &exc) {
    return core::Result<void>::Err(core::ErrorDTO{
        core::kErrorStartStateMismatch,
        std::string(
            "Could not publish observed state into the planning scene: ") +
            exc.what(),
        {{"reason", "pre_plan_scene_projection_failed"}},
    });
  }
  {
    std::lock_guard<std::mutex> execution_lock(execution_mutex_);
    if (running_) {
      return core::Result<void>::Err(core::ErrorDTO{
          core::kErrorInvalidRequest,
          "Observed planning-state synchronization raced with execution "
          "startup",
          {{"reason", "pre_plan_execution_already_running"}},
      });
    }
    (void)publish_observed_snapshot(
        core::ExecutionState::kIdle, 0.0, positions, velocities,
        sample->received_timestamp_ns, std::nullopt,
        running_generation_.load(std::memory_order_acquire));
  }
  return capture_planning_environment_authority();
}

std::optional<Ros2ControlExecutionBackend::JointSample>
Ros2ControlExecutionBackend::wait_for_fresh_passive_sample(double timeout_s) {
  const auto deadline =
      std::chrono::steady_clock::now() +
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::duration<double>(timeout_s));
  std::unique_lock<std::mutex> lock(state_mutex_);
  while (true) {
    const auto now = std::chrono::steady_clock::now();
    const JointSample *sample = select_latest_passive_sample_ptr_locked(now);
    if (sample != nullptr) {
      return *sample;
    }
    if (now >= deadline) {
      return std::nullopt;
    }
    state_cv_.wait_until(lock, deadline);
  }
}

std::optional<Ros2ControlExecutionBackend::JointSample>
Ros2ControlExecutionBackend::wait_for_fresh_passive_sample_after(
    std::chrono::steady_clock::time_point not_before,
    std::chrono::steady_clock::time_point deadline,
    std::stop_token stop_token) {
  std::unique_lock<std::mutex> lock(state_mutex_);
  while (true) {
    if (stop_token.stop_requested()) {
      return std::nullopt;
    }
    const auto now = std::chrono::steady_clock::now();
    auto sample = select_latest_passive_sample_after_locked(now, not_before);
    if (sample.has_value()) {
      return sample;
    }
    if (now >= deadline) {
      return std::nullopt;
    }
    // A permanently-false predicate makes condition_variable_any consume
    // every state notification internally and sleep until the deadline. Use
    // the actual sample condition so a controller/joint update after the FJT
    // result completes this handoff immediately. The stop-token overload
    // retains prompt cancellation without polling.
    state_cv_.wait_until(lock, stop_token, deadline, [this, not_before]() {
      return select_latest_passive_sample_after_locked(
                 std::chrono::steady_clock::now(), not_before)
          .has_value();
    });
  }
}

const std::vector<std::string> &
Ros2ControlExecutionBackend::transport_joint_names() const noexcept {
  return config_.transport_joint_names.empty() ? config_.required_joints
                                               : config_.transport_joint_names;
}

std::vector<double> Ros2ControlExecutionBackend::positions_for_joints(
    const JointSample &sample, const std::vector<std::string> &joints) const {
  std::vector<double> result;
  result.reserve(joints.size());
  for (const auto &joint : joints) {
    const auto it =
        std::find(sample.joint_names.begin(), sample.joint_names.end(), joint);
    if (it == sample.joint_names.end()) {
      throw std::runtime_error("sample missing joint " + joint);
    }
    const auto index =
        static_cast<size_t>(std::distance(sample.joint_names.begin(), it));
    if (index >= sample.positions.size()) {
      throw std::runtime_error("sample missing position for joint " + joint);
    }
    if (!std::isfinite(sample.positions[index])) {
      throw std::runtime_error("sample has non-finite position for joint " +
                               joint);
    }
    result.push_back(sample.positions[index]);
  }
  return result;
}

std::vector<double> Ros2ControlExecutionBackend::velocities_for_joints(
    const JointSample &sample, const std::vector<std::string> &joints) const {
  // Settle-signal helper: measured velocities aligned to `joints`, or an empty
  // vector when the sample carries no usable per-joint velocities. Empty means
  // settle UNKNOWN — callers MUST NOT treat it as at-rest.
  std::vector<double> result;
  if (sample.velocities.size() != sample.joint_names.size()) {
    return result;
  }
  result.reserve(joints.size());
  for (const auto &joint : joints) {
    const auto it =
        std::find(sample.joint_names.begin(), sample.joint_names.end(), joint);
    if (it == sample.joint_names.end()) {
      return {};
    }
    const auto index =
        static_cast<size_t>(std::distance(sample.joint_names.begin(), it));
    if (index >= sample.velocities.size()) {
      return {};
    }
    if (!std::isfinite(sample.velocities[index])) {
      return {};
    }
    result.push_back(sample.velocities[index]);
  }
  return result;
}

void Ros2ControlExecutionBackend::assign_joint_sample(
    JointSample &sample, const std::vector<std::string> &joint_names,
    const std::vector<double> &positions, const std::vector<double> &velocities,
    std::chrono::steady_clock::time_point received_at,
    std::int64_t received_timestamp_ns, const std::string &source,
    const std::string &topic) {
  sample.joint_names.assign(joint_names.begin(), joint_names.end());
  sample.positions.assign(positions.begin(), positions.end());
  sample.velocities.assign(velocities.begin(), velocities.end());
  sample.received_at = received_at;
  sample.received_timestamp_ns = received_timestamp_ns;
  sample.source = source;
  sample.topic = topic;
}

void Ros2ControlExecutionBackend::set_running_locked(
    bool running, ProgressRetirement progress_retirement,
    ResumeAuthorityRetention resume_authority_retention) noexcept {
  if (running_ != running) {
    // This generation fences goal callbacks, monitor snapshots, and authored
    // observed-state publication in both directions. In particular, terminal
    // completion must invalidate a monitor tick that began before completion.
    running_generation_.fetch_add(1, std::memory_order_acq_rel);
  }
  if (running) {
    active_action_feedback_authority_generation_.store(
        0, std::memory_order_release);
    running_flag_.store(true, std::memory_order_release);
  }
  running_ = running;
  if (!running) {
    active_action_feedback_authority_generation_.store(
        0, std::memory_order_release);
    running_flag_.store(false, std::memory_order_release);
    completion_gate_request_.reset();
    active_dispatch_authority_.reset();
    if (resume_authority_retention == ResumeAuthorityRetention::kDiscard) {
      resumable_dispatch_authority_.reset();
    }
    if (progress_retirement == ProgressRetirement::kRetireImmediately &&
        execution_progress_publication_ != nullptr) {
      execution_progress_publication_->invalidate();
    }
  }
}

void Ros2ControlExecutionBackend::testing_store_latest_joint_sample_impl(
    std::vector<std::string> joint_names, std::vector<double> positions,
    std::chrono::steady_clock::time_point received_at,
    std::vector<double> velocities,
    std::optional<std::int64_t> received_timestamp_ns) {
  const auto publication_received_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          received_at.time_since_epoch())
          .count();
  publish_observed_robot_state(core::ObservedRobotStateSourceKind::kJointState,
                               joint_names, positions, velocities, 0,
                               publication_received_ns);
  JointSample sample;
  sample.joint_names = std::move(joint_names);
  sample.positions = std::move(positions);
  sample.velocities = std::move(velocities);
  sample.received_at = received_at;
  sample.received_timestamp_ns =
      received_timestamp_ns.value_or(publication_received_ns);
  sample.source = "joint_state";
  sample.topic = config_.joint_state_topic;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    latest_joint_state_ = std::move(sample);
  }
  state_cv_.notify_all();
}

void Ros2ControlExecutionBackend::testing_store_latest_controller_sample_impl(
    std::vector<std::string> joint_names, std::vector<double> positions,
    std::chrono::steady_clock::time_point received_at,
    std::vector<double> velocities,
    std::optional<std::int64_t> received_timestamp_ns) {
  const auto publication_received_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          received_at.time_since_epoch())
          .count();
  publish_observed_robot_state(
      core::ObservedRobotStateSourceKind::kControllerState, joint_names,
      positions, velocities, 0, publication_received_ns);
  JointSample sample;
  sample.joint_names = std::move(joint_names);
  sample.positions = std::move(positions);
  sample.velocities = std::move(velocities);
  sample.received_at = received_at;
  sample.received_timestamp_ns =
      received_timestamp_ns.value_or(publication_received_ns);
  sample.source = "controller_state";
  sample.topic = config_.controller_state_topic;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    latest_controller_state_ = std::move(sample);
  }
  state_cv_.notify_all();
}

void Ros2ControlExecutionBackend::testing_complete_execution_impl() {
  std::optional<JointSample> observed_sample;
  if (!uses_authored_backend_state()) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    const auto *selected = select_latest_passive_sample_ptr_locked(
        std::chrono::steady_clock::now());
    if (selected != nullptr) {
      observed_sample = *selected;
    }
  }
  complete_execution(observed_sample ? &*observed_sample : nullptr);
}

Ros2ControlSceneProjectionMetrics
Ros2ControlExecutionBackend::scene_projection_metrics() const {
  Ros2ControlSceneProjectionMetrics metrics;
  const auto worker_metrics = scene_projection_worker_ != nullptr
                                  ? scene_projection_worker_->metrics()
                                  : detail::SceneProjectionWorkerMetrics{};
  metrics.misses_total =
      scene_projection_misses_total_.load(std::memory_order_relaxed) +
      worker_metrics.misses_total;
  metrics.successful_updates_total =
      scene_projection_successes_total_.load(std::memory_order_relaxed) +
      worker_metrics.successful_updates_total;
  metrics.lag_max_ns =
      std::max(scene_projection_lag_max_ns_.load(std::memory_order_relaxed),
               worker_metrics.lag_max_ns);
  metrics.last_success_timestamp_ns =
      std::max(scene_projection_last_success_timestamp_ns_.load(
                   std::memory_order_relaxed),
               worker_metrics.last_success_timestamp_ns);
  metrics.terminal_retries_scheduled_total =
      worker_metrics.terminal_retries_scheduled_total;
  metrics.terminal_retry_successes_total =
      worker_metrics.terminal_retry_successes_total;
  metrics.terminal_retry_exhausted_total =
      worker_metrics.terminal_retry_exhausted_total;
  metrics.active_requests_total = worker_metrics.active_requests_total;
  metrics.joint_requests_total = worker_metrics.joint_requests_total;
  metrics.requests_coalesced_total = worker_metrics.requests_coalesced_total;
  metrics.requests_discarded_total = worker_metrics.requests_discarded_total;
  metrics.requests_rejected_while_suspended_total =
      worker_metrics.requests_rejected_while_suspended_total;
  metrics.completion_callback_exceptions_total =
      worker_metrics.completion_callback_exceptions_total;
  metrics.apply_duration_max_ns = worker_metrics.apply_duration_max_ns;
  metrics.queue_lag_max_ns = worker_metrics.queue_lag_max_ns;
  const auto now_ns = clock_.now();
  if (metrics.last_success_timestamp_ns > 0 &&
      now_ns >= metrics.last_success_timestamp_ns) {
    metrics.last_success_age_ns =
        static_cast<std::uint64_t>(now_ns - metrics.last_success_timestamp_ns);
  }
  return metrics;
}

Ros2ControlControllerProgressMetrics
Ros2ControlExecutionBackend::controller_progress_metrics() const {
  Ros2ControlControllerProgressMetrics metrics;
  metrics.executor_thread_count = config_.executor_thread_count;
  metrics.state_callbacks_total =
      state_callbacks_total_.load(std::memory_order_relaxed);
  metrics.action_feedback_callbacks_total =
      action_feedback_callbacks_total_.load(std::memory_order_relaxed);
  metrics.action_result_callbacks_total =
      action_result_callbacks_total_.load(std::memory_order_relaxed);
  metrics.cancel_confirmations_total =
      cancel_confirmations_total_.load(std::memory_order_relaxed);
  metrics.stop_confirmations_total =
      stop_confirmations_total_.load(std::memory_order_relaxed);
  // Count/last/max form one observable sample publication. Read them under a
  // sequence counter so an API poll can never associate one command's count
  // with the next command's latency. These samples occur once per accepted
  // goal's first feedback, so the sequentially-consistent atomics are far
  // outside the controller-rate path and allocate no storage.
  for (;;) {
    const auto sequence_before =
        command_progress_metrics_sequence_.load(std::memory_order_seq_cst);
    if ((sequence_before & 1U) != 0U) {
      std::this_thread::yield();
      continue;
    }
    const auto samples_total =
        command_progress_samples_total_.load(std::memory_order_seq_cst);
    const auto last_latency =
        last_command_progress_latency_ns_.load(std::memory_order_seq_cst);
    const auto max_latency =
        max_command_progress_latency_ns_.load(std::memory_order_seq_cst);
    const auto sequence_after =
        command_progress_metrics_sequence_.load(std::memory_order_seq_cst);
    if (sequence_before == sequence_after) {
      metrics.command_progress_samples_total = samples_total;
      metrics.last_command_progress_latency_ns = last_latency;
      metrics.max_command_progress_latency_ns = max_latency;
      break;
    }
  }
  metrics.last_action_result_latency_ns =
      last_action_result_latency_ns_.load(std::memory_order_relaxed);
  metrics.max_action_result_latency_ns =
      max_action_result_latency_ns_.load(std::memory_order_relaxed);
  metrics.last_cancel_latency_ns =
      last_cancel_latency_ns_.load(std::memory_order_relaxed);
  metrics.max_cancel_latency_ns =
      max_cancel_latency_ns_.load(std::memory_order_relaxed);
  metrics.last_stop_confirmation_latency_ns =
      last_stop_confirmation_latency_ns_.load(std::memory_order_relaxed);
  metrics.max_stop_confirmation_latency_ns =
      max_stop_confirmation_latency_ns_.load(std::memory_order_relaxed);
  metrics.jtc_cancel_reference_publications_total =
      jtc_cancel_reference_publications_total_.load(std::memory_order_relaxed);
  metrics.jtc_cancel_reference_rejections_total =
      jtc_cancel_reference_rejections_total_.load(std::memory_order_relaxed);
  metrics.controller_binding_activity_events_total =
      controller_binding_activity_events_total_.load(std::memory_order_relaxed);
  metrics.controller_binding_refresh_attempts_total =
      controller_binding_refresh_attempts_total_.load(
          std::memory_order_relaxed);
  metrics.controller_binding_refresh_successes_total =
      controller_binding_refresh_successes_total_.load(
          std::memory_order_relaxed);
  metrics.controller_binding_refresh_failures_total =
      controller_binding_refresh_failures_total_.load(
          std::memory_order_relaxed);
  metrics.controller_binding_dispatch_rejections_total =
      controller_binding_dispatch_rejections_total_.load(
          std::memory_order_relaxed);
  metrics.execution_admission_dispatch_rejections_total =
      execution_admission_dispatch_rejections_total_.load(
          std::memory_order_relaxed);
  metrics.dispatch_authority_losses_total =
      dispatch_authority_losses_total_.load(std::memory_order_relaxed);
  if (observed_state_publication_ != nullptr) {
    const auto publication_metrics = observed_state_publication_->metrics();
    metrics.observed_state_reader_retry_exhausted_total =
        publication_metrics.reader_retry_exhausted_total;
    metrics.observed_state_published_total =
        publication_metrics.published_total;
    metrics.observed_state_rejected_total = publication_metrics.rejected_total;
    metrics.observed_state_writer_busy_total =
        publication_metrics.writer_busy_total;
    metrics.observed_state_slot_exhausted_total =
        publication_metrics.slot_exhausted_total;
    metrics.observed_state_sources =
        observed_state_publication_->source_metrics();
  }
  if (diagnostic_observed_state_publication_ != nullptr) {
    const auto diagnostic_metrics =
        diagnostic_observed_state_publication_->metrics();
    metrics.observed_state_reader_retry_exhausted_total +=
        diagnostic_metrics.reader_retry_exhausted_total;
    metrics.observed_state_published_total +=
        diagnostic_metrics.published_total;
    metrics.observed_state_rejected_total += diagnostic_metrics.rejected_total;
    metrics.observed_state_writer_busy_total +=
        diagnostic_metrics.writer_busy_total;
    metrics.observed_state_slot_exhausted_total +=
        diagnostic_metrics.slot_exhausted_total;
    auto diagnostic_sources =
        diagnostic_observed_state_publication_->source_metrics();
    metrics.observed_state_sources.insert(metrics.observed_state_sources.end(),
                                          diagnostic_sources.begin(),
                                          diagnostic_sources.end());
  }
  metrics.authored_active_publications_total =
      authored_active_publications_total_.load(std::memory_order_relaxed);
  metrics.authored_active_interarrival_max_ns =
      authored_active_interarrival_max_ns_.load(std::memory_order_relaxed);

  const auto now = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lock(state_mutex_);
  const JointSample *sample = select_latest_sample_ptr_locked(now);
  if (sample != nullptr) {
    metrics.latest_state_sample_age_ns =
        static_cast<std::uint64_t>(std::max<std::int64_t>(
            0, std::chrono::duration_cast<std::chrono::nanoseconds>(
                   now - sample->received_at)
                   .count()));
  }
  return metrics;
}

bool Ros2ControlExecutionBackend::publish_execution_admission_snapshot(
    ExecutionAdmissionSnapshot snapshot) {
  if (snapshot.generation == 0 || snapshot.lifecycle_generation == 0 ||
      snapshot.robot_id.empty() || snapshot.runtime_generation_id.empty() ||
      snapshot.provider_component_epoch.empty() ||
      snapshot.cell_supervisor_component_epoch.empty() ||
      (snapshot.motion_admitted &&
       (snapshot.transport_owner_component_epoch.empty() ||
        snapshot.authority_component_epoch.empty())) ||
      snapshot.observed_timestamp_ns <= 0 ||
      snapshot.received_timestamp_ns <= 0 ||
      snapshot.observed_timestamp_ns > snapshot.received_timestamp_ns ||
      snapshot.fresh_until_ns <= snapshot.received_timestamp_ns ||
      (snapshot.motion_admitted && !snapshot.controller_ready) ||
      (config_.controller_binding.has_value() &&
       snapshot.binding_contract_generation_id !=
           config_.controller_binding->generation_id) ||
      std::any_of(snapshot.reason_codes.begin(), snapshot.reason_codes.end(),
                  [](const std::string &reason) { return reason.empty(); })) {
    return false;
  }
  std::sort(snapshot.reason_codes.begin(), snapshot.reason_codes.end());
  if (std::adjacent_find(snapshot.reason_codes.begin(),
                         snapshot.reason_codes.end()) !=
      snapshot.reason_codes.end()) {
    return false;
  }
  auto immutable =
      std::make_shared<const ExecutionAdmissionSnapshot>(std::move(snapshot));
  {
    std::lock_guard<std::mutex> lock(dispatch_admission_mutex_);
    const auto current =
        execution_admission_snapshot_.load(std::memory_order_acquire);
    if (current != nullptr) {
      if (immutable->generation < current->generation) {
        return false;
      }
      if (immutable->generation == current->generation) {
        return *immutable == *current;
      }
      if (immutable->robot_id != current->robot_id ||
          immutable->binding_contract_generation_id !=
              current->binding_contract_generation_id ||
          immutable->runtime_generation_id != current->runtime_generation_id ||
          immutable->provider_component_epoch !=
              current->provider_component_epoch ||
          immutable->cell_supervisor_component_epoch !=
              current->cell_supervisor_component_epoch ||
          immutable->lifecycle_generation < current->lifecycle_generation ||
          immutable->observed_timestamp_ns < current->observed_timestamp_ns ||
          immutable->received_timestamp_ns < current->received_timestamp_ns) {
        return false;
      }
      if (immutable->lifecycle_generation == current->lifecycle_generation &&
          (immutable->controller_ready != current->controller_ready ||
           immutable->motion_admitted != current->motion_admitted ||
           immutable->transport_owner_component_epoch !=
               current->transport_owner_component_epoch ||
           immutable->authority_component_epoch !=
               current->authority_component_epoch ||
           immutable->reason_codes != current->reason_codes)) {
        // A publication generation may renew timestamps/lease, but changing
        // lifecycle semantics requires a new lifecycle generation.
        return false;
      }
    }
    execution_admission_snapshot_.store(std::move(immutable),
                                        std::memory_order_release);
  }
  execution_cv_.notify_all();
  return true;
}

std::optional<ExecutionAdmissionSnapshot>
Ros2ControlExecutionBackend::execution_admission_snapshot() const {
  const auto snapshot =
      execution_admission_snapshot_.load(std::memory_order_acquire);
  if (snapshot == nullptr) {
    return std::nullopt;
  }
  return *snapshot;
}

std::optional<ControllerBindingSnapshot>
Ros2ControlExecutionBackend::controller_binding_snapshot() const {
  const auto snapshot =
      controller_binding_snapshot_.load(std::memory_order_acquire);
  if (snapshot == nullptr) {
    return std::nullopt;
  }
  return *snapshot;
}

DispatchAdmissionDecision
Ros2ControlExecutionBackend::current_dispatch_admission(
    const std::vector<std::string> &trajectory_joint_names) const {
  std::lock_guard<std::mutex> lock(dispatch_admission_mutex_);
  const auto binding =
      controller_binding_snapshot_.load(std::memory_order_acquire);
  const auto admission =
      execution_admission_snapshot_.load(std::memory_order_acquire);
  return evaluate_dispatch_admission(
      config_.controller_binding.has_value() ? &*config_.controller_binding
                                             : nullptr,
      binding.get(), admission.get(),
      config_.require_execution_admission_snapshot, clock_.now(),
      config_.action_name, trajectory_joint_names);
}

DispatchAdmissionDecision
Ros2ControlExecutionBackend::current_active_dispatch_authority(
    const DispatchAuthorityPin &pin) const {
  if (pin.execution_backend_component_epoch !=
      execution_backend_component_epoch_) {
    DispatchAdmissionDecision decision;
    decision.reason_code = "execution_backend_epoch_changed";
    decision.message =
        "The generic execution backend changed after trajectory dispatch";
    return decision;
  }
  std::lock_guard<std::mutex> lock(dispatch_admission_mutex_);
  return current_active_dispatch_authority_locked(pin);
}

DispatchAdmissionDecision
Ros2ControlExecutionBackend::current_active_dispatch_authority_locked(
    const DispatchAuthorityPin &pin) const {
  if (pin.execution_backend_component_epoch !=
      execution_backend_component_epoch_) {
    DispatchAdmissionDecision decision;
    decision.reason_code = "execution_backend_epoch_changed";
    decision.message =
        "The generic execution backend changed after trajectory dispatch";
    return decision;
  }
  const auto binding =
      controller_binding_snapshot_.load(std::memory_order_acquire);
  const auto admission =
      execution_admission_snapshot_.load(std::memory_order_acquire);
  return evaluate_active_dispatch_authority(
      config_.controller_binding.has_value() ? &*config_.controller_binding
                                             : nullptr,
      binding.get(), admission.get(),
      config_.require_execution_admission_snapshot, clock_.now(), pin);
}

core::Result<void>
Ros2ControlExecutionBackend::capture_planning_environment_authority() {
  if (!config_.controller_binding.has_value() &&
      !config_.require_execution_admission_snapshot) {
    return core::Result<void>::Ok();
  }
  std::lock_guard<std::mutex> lock(dispatch_admission_mutex_);
  const auto binding =
      controller_binding_snapshot_.load(std::memory_order_acquire);
  const auto admission =
      execution_admission_snapshot_.load(std::memory_order_acquire);
  auto decision = evaluate_dispatch_admission(
      config_.controller_binding.has_value() ? &*config_.controller_binding
                                             : nullptr,
      binding.get(), admission.get(),
      config_.require_execution_admission_snapshot, clock_.now(),
      config_.action_name, transport_joint_names());
  if (!decision.admitted || !decision.pin.has_value()) {
    return core::Result<void>::Err(core::ErrorDTO{
        core::kErrorExecutionRejected,
        decision.message.empty()
            ? "Execution environment was not admitted before planning"
            : std::move(decision.message),
        {{"phase", "pre_planning_execution_environment_pin"},
         {"reason", decision.reason_code}},
    });
  }
  decision.pin->execution_backend_component_epoch =
      execution_backend_component_epoch_;
  planning_environment_authority_ = std::move(*decision.pin);
  return core::Result<void>::Ok();
}

bool Ros2ControlExecutionBackend::install_dispatch_authority_pin(
    const DispatchAuthorityPin &pin, std::uint64_t dispatch_generation) {
  std::lock_guard<std::mutex> lock(execution_mutex_);
  if (!running_ || terminal_ ||
      running_generation_.load(std::memory_order_acquire) !=
          dispatch_generation) {
    return false;
  }
  active_dispatch_authority_ = pin;
  resumable_dispatch_authority_ = pin;
  return true;
}

void Ros2ControlExecutionBackend::fail_active_dispatch_authority(
    DispatchAdmissionDecision decision) {
  GoalHandleFJT::SharedPtr handle_to_cancel;
  {
    std::lock_guard<std::mutex> lock(execution_mutex_);
    if (!running_ || terminal_) {
      return;
    }
    handle_to_cancel = active_goal_handle_;
  }
  dispatch_authority_losses_total_.fetch_add(1, std::memory_order_relaxed);
  cancel_goal_if_known(handle_to_cancel);
  decision.details["admission_failure_reason"] = decision.reason_code;
  fail_execution(decision.message.empty()
                     ? "Controller binding or lifecycle admission was lost"
                     : std::move(decision.message),
                 core::ErrorCode::kExecutionAborted,
                 decision.reason_code.rfind("controller_", 0) == 0
                     ? ros2_control_fault_reason::kControllerBindingLost
                     : ros2_control_fault_reason::kExecutionAdmissionLost,
                 std::move(decision.details));
}

void Ros2ControlExecutionBackend::record_scene_projection_success(
    std::int64_t source_timestamp_ns) {
  const auto now_ns = clock_.now();
  scene_projection_successes_total_.fetch_add(1, std::memory_order_relaxed);
  scene_projection_last_success_timestamp_ns_.store(now_ns,
                                                    std::memory_order_relaxed);
  if (source_timestamp_ns > 0 && now_ns >= source_timestamp_ns) {
    atomic_max(scene_projection_lag_max_ns_,
               static_cast<std::uint64_t>(now_ns - source_timestamp_ns));
  }
}

void Ros2ControlExecutionBackend::record_scene_projection_miss() {
  scene_projection_misses_total_.fetch_add(1, std::memory_order_relaxed);
}

void Ros2ControlExecutionBackend::record_command_progress_latency_ns(
    std::int64_t latency_ns) {
  begin_command_progress_metrics_publication(latency_ns);
  finish_command_progress_metrics_publication();
}

void Ros2ControlExecutionBackend::begin_command_progress_metrics_publication(
    std::int64_t latency_ns) {
  command_progress_metrics_sequence_.fetch_add(1, std::memory_order_seq_cst);
  const auto clamped =
      static_cast<std::uint64_t>(std::max<std::int64_t>(0, latency_ns));
  last_command_progress_latency_ns_.store(clamped, std::memory_order_seq_cst);
  atomic_max_sequentially_consistent(max_command_progress_latency_ns_, clamped);
  command_progress_samples_total_.fetch_add(1, std::memory_order_seq_cst);
}

void Ros2ControlExecutionBackend::
    finish_command_progress_metrics_publication() {
  command_progress_metrics_sequence_.fetch_add(1, std::memory_order_seq_cst);
}

void Ros2ControlExecutionBackend::record_action_result_latency_ns(
    std::int64_t latency_ns) {
  record_latency_value(last_action_result_latency_ns_,
                       max_action_result_latency_ns_, latency_ns);
}

void Ros2ControlExecutionBackend::record_cancel_latency_ns(
    std::int64_t latency_ns) {
  record_latency_sample(last_cancel_latency_ns_, max_cancel_latency_ns_,
                        cancel_confirmations_total_, latency_ns);
}

void Ros2ControlExecutionBackend::record_stop_confirmation_latency_ns(
    std::int64_t latency_ns) {
  record_latency_sample(last_stop_confirmation_latency_ns_,
                        max_stop_confirmation_latency_ns_,
                        stop_confirmations_total_, latency_ns);
}

bool Ros2ControlExecutionBackend::update_scene_positions(
    const std::vector<double> &joint_positions, SceneUpdateMode mode,
    std::int64_t source_timestamp_ns) {
  if (joint_positions.empty()) {
    return true;
  }
  std::unique_lock<std::mutex> barrier_lock;
  detail::SceneProjectionWorker::Suspension suspension;
  if (mode == SceneUpdateMode::kBlocking &&
      scene_projection_worker_ != nullptr) {
    barrier_lock =
        std::unique_lock<std::mutex>(scene_projection_barrier_mutex_);
    suspension = scene_projection_worker_->suspend();
    if (!scene_projection_worker_->drain_for(kSceneProjectionBarrierTimeout)) {
      throw std::runtime_error(
          "timed out draining asynchronous scene projection before a blocking "
          "scene-state update");
    }
  }
  const auto sink_mode = mode == SceneUpdateMode::kBestEffort
                             ? core::ExecutionStateUpdateMode::kBestEffort
                             : core::ExecutionStateUpdateMode::kBlocking;
  const auto result = execution_state_sink_->apply_group_joint_positions(
      group_name_, joint_positions, sink_mode);
  if (result.applied()) {
    record_scene_projection_success(source_timestamp_ns);
    return true;
  }
  record_scene_projection_miss();
  if (mode == SceneUpdateMode::kBestEffort) {
    return false;
  }
  throw std::runtime_error(
      "failed to update execution state sink: " +
      std::string(core::execution_state_sink_status_reason(result.status)));
}

void Ros2ControlExecutionBackend::project_terminal_scene_positions(
    const std::vector<double> &joint_positions,
    std::int64_t source_timestamp_ns) {
  if (scene_projection_worker_ != nullptr && !joint_positions.empty()) {
    (void)scene_projection_worker_->submit_terminal(joint_positions,
                                                    source_timestamp_ns);
  }
}

bool Ros2ControlExecutionBackend::schedule_active_scene_projection(
    std::vector<double> joint_positions, std::int64_t source_timestamp_ns) {
  if (scene_projection_worker_ != nullptr && !joint_positions.empty()) {
    return scene_projection_worker_->submit_active(std::move(joint_positions),
                                                   source_timestamp_ns);
  }
  return joint_positions.empty();
}

void Ros2ControlExecutionBackend::stop_scene_projection_worker() noexcept {
  if (scene_projection_worker_ != nullptr &&
      !scene_projection_worker_->stop_and_join_for(std::chrono::seconds(1))) {
    RCLCPP_FATAL(
        rclcpp::get_logger("manyforge_ros2_control_execution"),
        "Timed out draining the NRT scene-projection worker during backend "
        "destruction");
    std::terminate();
  }
}

bool Ros2ControlExecutionBackend::drain_scene_projection_for(
    std::chrono::nanoseconds timeout) {
  return scene_projection_worker_ == nullptr ||
         scene_projection_worker_->drain_for(timeout);
}

std::optional<double> Ros2ControlExecutionBackend::scene_group_joint_position(
    const std::string &group_id, const std::string &joint_name) const {
  return execution_state_sink_->group_joint_position(group_id, joint_name);
}

bool Ros2ControlExecutionBackend::schedule_gripper_scene_projection_locked(
    const std::shared_ptr<GripperRuntime> &runtime, double position,
    bool terminal) {
  if (!runtime || !std::isfinite(position)) {
    return false;
  }
  if (runtime->config.group_id.empty() || runtime->config.joint_name.empty()) {
    runtime->projection_state = core::GripperProjectionState::kNotRequired;
    runtime->projection_reason.clear();
    return true;
  }
  if (scene_projection_worker_ == nullptr ||
      !runtime->scene_projection_slot.has_value()) {
    runtime->projection_state = core::GripperProjectionState::kPending;
    runtime->projection_reason = "scene_projection_worker_unavailable";
    return false;
  }

  const detail::SceneProjectionToken token{
      runtime->command_id,
      runtime->generation,
      ++runtime->scene_projection_request_id,
  };
  const bool queued =
      terminal
          ? scene_projection_worker_->submit_terminal_joint(
                *runtime->scene_projection_slot, position, clock_.now(), token)
          : scene_projection_worker_->submit_active_joint(
                *runtime->scene_projection_slot, position, clock_.now(), token);
  runtime->projection_state = core::GripperProjectionState::kPending;
  runtime->projection_reason =
      queued ? "scene_projection_queued" : "scene_projection_suspended";
  return queued;
}

void Ros2ControlExecutionBackend::on_gripper_scene_projection_complete(
    const std::weak_ptr<GripperRuntime> &weak_runtime,
    const detail::SceneProjectionCompletion &completion) noexcept {
  const auto runtime = weak_runtime.lock();
  if (!runtime) {
    return;
  }

  bool retry_superseded_terminal = false;
  std::uint64_t command_id = 0;
  std::uint64_t generation = 0;
  {
    std::lock_guard<std::mutex> lock(runtime->mutex);
    if (runtime->command_id != completion.token.command_id ||
        runtime->generation != completion.token.generation ||
        runtime->scene_projection_request_id != completion.token.request_id) {
      return;
    }
    command_id = runtime->command_id;
    generation = runtime->generation;
    switch (completion.outcome) {
    case detail::SceneProjectionOutcome::kApplied:
      runtime->projection_state = core::GripperProjectionState::kApplied;
      runtime->projection_reason.clear();
      runtime->projection_retry_count = 0;
      break;
    case detail::SceneProjectionOutcome::kMissed:
      runtime->projection_state = core::GripperProjectionState::kPending;
      runtime->projection_reason = "scene_sink_busy";
      break;
    case detail::SceneProjectionOutcome::kRetryExhausted:
      runtime->projection_state = core::GripperProjectionState::kFailed;
      runtime->projection_reason = "scene_sink_retry_exhausted";
      runtime->state = "failed";
      runtime->executing = false;
      runtime->error =
          "Timed out publishing the terminal gripper scene position";
      runtime->reason_code = "gripper_projection_failed";
      break;
    case detail::SceneProjectionOutcome::kSuperseded:
      runtime->projection_state = core::GripperProjectionState::kPending;
      runtime->projection_reason = "scene_projection_superseded";
      retry_superseded_terminal =
          !runtime->executing && runtime->state == "completed";
      break;
    }
    ++runtime->revision;
  }
  if (retry_superseded_terminal) {
    try {
      (void)schedule_gripper_lifecycle(runtime, command_id, generation,
                                       clock_.now() + 20'000'000);
    } catch (...) {
      // The generation remains conservatively pending. Reset/replacement can
      // still invalidate it; a scheduler exception must not escape this NRT
      // completion callback or terminate the projection worker.
    }
  }
}

bool Ros2ControlExecutionBackend::publish_snapshot(
    core::ExecutionState state, double progress,
    const std::vector<double> &joint_positions,
    std::optional<core::ErrorDTO> error, std::string state_source,
    std::optional<std::uint64_t> expected_running_generation,
    SnapshotPublicationAuthority authority,
    const std::vector<double> *authored_velocities) {
  const auto now_ns = clock_.now();
  if (state_source.empty()) {
    if (is_commanded_sim_mode()) {
      state_source = "commanded_sim_predicted";
    } else if (config_.execution_state_source_mode == "authored_preview") {
      state_source = "authored_preview";
    } else {
      state_source = "observed_ros2";
    }
  }
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (expected_running_generation.has_value() &&
        running_generation_.load(std::memory_order_acquire) !=
            *expected_running_generation) {
      return false;
    }
    snapshot_.timestamp_ns = now_ns;
    snapshot_.sequence_number++;
    snapshot_.state = state;
    snapshot_.progress = clamp_progress(progress);
    snapshot_.joint_positions_rad = joint_positions;
    // Velocities are settle evidence and are only valid for the exact state
    // just published. Reset here so a position-only publish never leaves stale
    // velocities; the controlled-stop path sets measured velocities explicitly
    // after publishing the post-stop snapshot.
    snapshot_.joint_velocities_rad_s.clear();
    snapshot_.state_source = std::move(state_source);
    snapshot_.source_timestamp_ns = now_ns;
    snapshot_.source_age_ns = 0;
    snapshot_.error = std::move(error);
  }

  if (uses_authored_backend_state() &&
      authority == SnapshotPublicationAuthority::kBackendAuthored &&
      !joint_positions.empty()) {
    const bool at_rest_state =
        state == core::ExecutionState::kIdle ||
        state == core::ExecutionState::kPaused ||
        state == core::ExecutionState::kStopped ||
        state == core::ExecutionState::kCompleted ||
        state == core::ExecutionState::kFailed ||
        state == core::ExecutionState::kInterventionRequired;
    const std::span<const double> velocities =
        authored_velocities != nullptr
            ? std::span<const double>(*authored_velocities)
            : (at_rest_state
                   ? std::span<const double>(authored_at_rest_velocities_)
                   : std::span<const double>{});
    const bool authority_boundary = !expected_running_generation.has_value();
    const bool authored_published = publish_authored_robot_state(
        joint_positions, velocities, now_ns, expected_running_generation,
        authority_boundary);
    if (!authored_published &&
        (!expected_running_generation.has_value() ||
         running_generation_.load(std::memory_order_acquire) ==
             *expected_running_generation) &&
        node_) {
      RCLCPP_ERROR_THROTTLE(
          node_->get_logger(), *node_->get_clock(), 5000,
          "Failed to publish backend-authored robot state; planning authority "
          "remains invalid until an authoritative replacement is published");
    }
    return authored_published;
  }
  if (uses_authored_backend_state() &&
      authority == SnapshotPublicationAuthority::kBackendAuthored &&
      joint_positions.empty() && !expected_running_generation.has_value()) {
    std::lock_guard<std::mutex> writer_lock(authored_state_publication_mutex_);
    const auto authority_epoch =
        authored_state_authority_epoch_.load(std::memory_order_relaxed);
    if ((authority_epoch & 1u) == 0u) {
      authored_state_authority_epoch_.store(authority_epoch + 1,
                                            std::memory_order_release);
    }
    return false;
  }
  return true;
}

bool Ros2ControlExecutionBackend::publish_passive_observed_snapshot(
    const std::vector<double> &joint_positions,
    const std::vector<double> &joint_velocities,
    std::int64_t source_timestamp_ns,
    std::uint64_t expected_running_generation) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (running_flag_.load(std::memory_order_acquire) ||
      running_generation_.load(std::memory_order_acquire) !=
          expected_running_generation) {
    return false;
  }
  switch (snapshot_.state) {
  case core::ExecutionState::kIdle:
  case core::ExecutionState::kPaused:
  case core::ExecutionState::kStopped:
  case core::ExecutionState::kCompleted:
  case core::ExecutionState::kFailed:
  case core::ExecutionState::kInterventionRequired:
    break;
  case core::ExecutionState::kPlanning:
  case core::ExecutionState::kValidating:
  case core::ExecutionState::kExecuting:
  case core::ExecutionState::kInterrupted:
  case core::ExecutionState::kStopping:
    return false;
  }
  if (snapshot_.state_source == "observed_ros2" &&
      snapshot_.source_timestamp_ns > source_timestamp_ns) {
    return false;
  }
  const auto now_ns = clock_.now();
  snapshot_.timestamp_ns = now_ns;
  snapshot_.sequence_number++;
  snapshot_.joint_positions_rad = joint_positions;
  snapshot_.joint_velocities_rad_s = joint_velocities;
  snapshot_.state_source = "observed_ros2";
  snapshot_.source_timestamp_ns = source_timestamp_ns;
  snapshot_.source_age_ns =
      now_ns >= source_timestamp_ns ? now_ns - source_timestamp_ns : 0;
  return true;
}

bool Ros2ControlExecutionBackend::publish_observed_snapshot(
    core::ExecutionState state, double progress,
    const std::vector<double> &joint_positions,
    const std::vector<double> &joint_velocities,
    std::int64_t source_timestamp_ns, std::optional<core::ErrorDTO> error,
    std::uint64_t expected_running_generation) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (running_generation_.load(std::memory_order_acquire) !=
      expected_running_generation) {
    return false;
  }
  if (snapshot_.state_source == "observed_ros2" &&
      snapshot_.source_timestamp_ns > source_timestamp_ns) {
    return false;
  }
  const auto now_ns = clock_.now();
  snapshot_.timestamp_ns = now_ns;
  snapshot_.sequence_number++;
  snapshot_.state = state;
  snapshot_.progress = clamp_progress(progress);
  snapshot_.joint_positions_rad = joint_positions;
  snapshot_.joint_velocities_rad_s = joint_velocities;
  snapshot_.state_source = "observed_ros2";
  snapshot_.source_timestamp_ns = source_timestamp_ns;
  snapshot_.source_age_ns =
      now_ns >= source_timestamp_ns ? now_ns - source_timestamp_ns : 0;
  snapshot_.error = std::move(error);
  return true;
}

bool Ros2ControlExecutionBackend::publish_observed_metadata(
    core::ExecutionState state, double progress,
    std::optional<core::ErrorDTO> error,
    std::uint64_t expected_running_generation) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (running_generation_.load(std::memory_order_acquire) !=
      expected_running_generation) {
    return false;
  }
  snapshot_.timestamp_ns = clock_.now();
  snapshot_.sequence_number++;
  snapshot_.state = state;
  snapshot_.progress = clamp_progress(progress);
  // Position, velocity, source and source timestamp deliberately remain
  // unchanged. In observed_ros2 mode an execution-state transition is
  // metadata; it cannot manufacture a newer physical pose.
  snapshot_.error = std::move(error);
  return true;
}

bool Ros2ControlExecutionBackend::install_controlled_stop_command_reference(
    const core::TrajectoryDTO &trajectory,
    std::chrono::steady_clock::time_point started_at) {
  auto valid = core::validate_trajectory_for_sampling(trajectory);
  if (!valid.ok()) {
    if (node_) {
      RCLCPP_WARN_THROTTLE(
          node_->get_logger(), *node_->get_clock(), 5000,
          "trajectory_sampler validation rejected Ros2ControlExecutionBackend "
          "controlled-stop command-reference: %s (waypoints=%zu)",
          valid.error().code.c_str(), trajectory.waypoints.size());
    }
    std::lock_guard<std::mutex> lock(execution_mutex_);
    controlled_stop_command_reference_.reset();
    execution_cv_.notify_all();
    return false;
  }
  auto reference = std::make_shared<ControlledStopCommandReference>();
  reference->trajectory = trajectory;
  reference->started_at = started_at;
  std::lock_guard<std::mutex> lock(execution_mutex_);
  controlled_stop_preempt_in_progress_ = true;
  controlled_stop_command_reference_ = std::move(reference);
  execution_cv_.notify_all();
  return true;
}

void Ros2ControlExecutionBackend::clear_managed_controlled_stop_state() {
  std::lock_guard<std::mutex> lock(execution_mutex_);
  controlled_stop_preempt_in_progress_ = false;
  controlled_stop_command_reference_.reset();
}

bool Ros2ControlExecutionBackend::complete_forced_managed_stop_execution(
    const std::uint64_t expected_running_generation) {
  core::ExecutionSnapshot terminal_snapshot;
  std::uint64_t terminal_generation = 0U;
  {
    std::lock_guard<std::mutex> lock(execution_mutex_);
    if (!running_ || terminal_ ||
        running_generation_.load(std::memory_order_acquire) !=
            expected_running_generation) {
      return false;
    }
    terminal_snapshot = get_execution_snapshot();
    terminal_ = true;
    set_running_locked(false);
    terminal_generation =
        running_generation_.load(std::memory_order_acquire);
    stop_requested_ = false;
    natural_completion_stop_requested_ = false;
    pause_requested_ = false;
    active_goal_handle_.reset();
    active_goal_id_.reset();
    active_trajectory_.reset();
    active_trajectory_time_s_ = 0.0;
    active_runtime_velocity_scale_ = 1.0;
    controlled_stop_preempt_in_progress_ = false;
    controlled_stop_command_reference_.reset();
    if (uses_authored_backend_state()) {
      (void)publish_snapshot(
          core::ExecutionState::kStopped, terminal_snapshot.progress,
          terminal_snapshot.joint_positions_rad, std::nullopt,
          "commanded_sim_forced_managed_stop", terminal_generation,
          SnapshotPublicationAuthority::kBackendAuthored);
    } else {
      (void)publish_observed_metadata(
          core::ExecutionState::kStopped, terminal_snapshot.progress,
          std::nullopt, terminal_generation);
    }
  }
  if (!terminal_snapshot.joint_positions_rad.empty()) {
    project_terminal_scene_positions(
        terminal_snapshot.joint_positions_rad,
        uses_authored_backend_state() ? clock_.now()
                                      : terminal_snapshot.source_timestamp_ns);
  }
  execution_cv_.notify_all();
  return true;
}

bool Ros2ControlExecutionBackend::
    publish_controlled_stop_command_reference_snapshot(
        std::chrono::steady_clock::time_point now,
        std::optional<std::uint64_t> expected_running_generation) {
  if (!uses_authored_backend_state()) {
    // The replacement trajectory is controller intent. It is authoritative
    // for commanded simulation, but it must never become the reported robot
    // pose or planning-scene pose on a measured real-robot lane.
    return false;
  }
  std::shared_ptr<ControlledStopCommandReference> reference;
  {
    std::lock_guard<std::mutex> lock(execution_mutex_);
    reference = controlled_stop_command_reference_;
  }
  if (!reference || reference->trajectory.waypoints.empty()) {
    return false;
  }

  const double elapsed_s = std::max(
      0.0, std::chrono::duration<double>(now - reference->started_at).count());
  const double end_s =
      std::max(0.0, reference->trajectory.waypoints.back().time_from_start_s);
  const double sample_time_s = std::min(elapsed_s, end_s);
  auto sample = core::sample_trajectory_prevalidated(
      reference->trajectory, sample_time_s, reference->sample);
  if (!sample.ok() || reference->sample.positions_rad.empty()) {
    if (node_) {
      RCLCPP_WARN_THROTTLE(
          node_->get_logger(), *node_->get_clock(), 5000,
          "trajectory_sampler error in Ros2ControlExecutionBackend "
          "controlled-stop "
          "command-reference: %s (time_s=%.3f, waypoints=%zu)",
          sample.ok() ? "empty_positions" : sample.error().code.c_str(),
          sample_time_s, reference->trajectory.waypoints.size());
    }
    return false;
  }

  const double progress =
      end_s <= 1e-9 ? 1.0 : clamp_progress(sample_time_s / end_s);
  const bool published = publish_snapshot(
      core::ExecutionState::kStopping, progress,
      reference->sample.positions_rad, std::nullopt,
      is_commanded_sim_mode() ? "commanded_sim_controlled_stop_reference"
                              : "controlled_stop_command_reference",
      expected_running_generation,
      SnapshotPublicationAuthority::kBackendAuthored,
      &reference->sample.velocities_rad_s);
  if (published) {
    (void)schedule_active_scene_projection(reference->sample.positions_rad,
                                           clock_.now());
  }
  return published;
}

bool Ros2ControlExecutionBackend::publish_observed_cancel_stop_snapshot(
    const std::vector<std::string> &joint_names, double progress,
    std::chrono::steady_clock::time_point now,
    std::optional<std::uint64_t> expected_running_generation) {
  std::vector<double> positions;
  std::vector<double> velocities;
  std::int64_t source_timestamp_ns = 0;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    const auto sample = select_latest_sample_locked(now);
    if (!sample.has_value()) {
      return false;
    }
    try {
      positions = positions_for_joints(*sample, transport_joint_names());
      velocities = velocities_for_joints(*sample, transport_joint_names());
      source_timestamp_ns = sample->received_timestamp_ns;
    } catch (const std::exception &) {
      return false;
    }
  }
  if (positions.empty() || positions.size() != joint_names.size()) {
    return false;
  }

  const auto generation = expected_running_generation.value_or(
      running_generation_.load(std::memory_order_acquire));
  if (!publish_observed_snapshot(core::ExecutionState::kStopping, progress,
                                 positions, velocities, source_timestamp_ns,
                                 std::nullopt, generation)) {
    return false;
  }
  (void)schedule_active_scene_projection(positions, source_timestamp_ns);
  return true;
}

std::optional<Ros2ControlExecutionBackend::PostStopObservation>
Ros2ControlExecutionBackend::wait_for_post_stop_observation(
    const std::vector<std::string> &joint_names,
    const std::vector<double> &settle_velocity_thresholds_rad_s,
    std::chrono::steady_clock::time_point not_before,
    std::chrono::steady_clock::time_point deadline,
    std::stop_token stop_token) {
  if (joint_names.empty() ||
      settle_velocity_thresholds_rad_s.size() != joint_names.size()) {
    return std::nullopt;
  }
  std::unique_lock<std::mutex> lock(state_mutex_);
  std::optional<PostStopObservation> best;
  while (true) {
    if (stop_token.stop_requested()) {
      return best;
    }
    const auto now = std::chrono::steady_clock::now();
    const JointSample *sample = nullptr;
    for (const auto &source : config_.observed_state_source_precedence) {
      if (source == "controller_state" &&
          latest_controller_state_.has_value() &&
          joint_sample_is_fresh_and_valid(*latest_controller_state_, now,
                                          not_before)) {
        sample = &*latest_controller_state_;
        break;
      }
      if (source == "joint_state" && latest_joint_state_.has_value() &&
          joint_sample_is_fresh_and_valid(*latest_joint_state_, now,
                                          not_before)) {
        sample = &*latest_joint_state_;
        break;
      }
    }
    if (sample != nullptr) {
      PostStopObservation observation;
      try {
        observation.positions =
            positions_for_joints(*sample, transport_joint_names());
        observation.velocities =
            velocities_for_joints(*sample, transport_joint_names());
      } catch (const std::exception &) {
        sample = nullptr;
      }
      if (sample != nullptr &&
          observation.positions.size() == joint_names.size()) {
        observation.source = sample->source;
        observation.topic = sample->topic;
        observation.received_timestamp_ns = sample->received_timestamp_ns;
        observation.observed_at = sample->received_at;
        observation.velocity_available =
            observation.velocities.size() == joint_names.size();
        if (observation.velocity_available) {
          observation.at_rest = true;
          for (std::size_t index = 0U; index < observation.velocities.size();
               ++index) {
            const double velocity = observation.velocities[index];
            observation.max_abs_velocity_rad_s = std::max(
                observation.max_abs_velocity_rad_s, std::abs(velocity));
            if (std::abs(velocity) > settle_velocity_thresholds_rad_s[index]) {
              observation.at_rest = false;
            }
          }
        }
        // The deployment-declared source precedence is authoritative. Keep
        // only successive samples from that selected source; do not pick a
        // lower-precedence source merely because it reports a smaller speed.
        best = observation;
        if (observation.velocity_available && observation.at_rest) {
          return best;
        }
      }
    }
    if (now >= deadline) {
      return best;
    }
    const auto next_wakeup =
        std::min(deadline, now + std::chrono::milliseconds(20));
    state_cv_.wait_for(lock, stop_token, next_wakeup - now,
                       []() { return false; });
  }
}

std::uint64_t
Ros2ControlExecutionBackend::complete_managed_controlled_stop_execution() {
  std::uint64_t terminal_generation = 0;
  {
    std::lock_guard<std::mutex> lock(execution_mutex_);
    terminal_ = true;
    set_running_locked(false);
    stop_requested_ = false;
    natural_completion_stop_requested_ = false;
    pause_requested_ = false;
    active_goal_handle_.reset();
    active_goal_id_.reset();
    active_trajectory_.reset();
    active_trajectory_time_s_ = 0.0;
    active_runtime_velocity_scale_ = 1.0;
    controlled_stop_preempt_in_progress_ = false;
    controlled_stop_command_reference_.reset();
    terminal_generation = running_generation_.load(std::memory_order_acquire);
  }
  execution_cv_.notify_all();
  return terminal_generation;
}

core::ErrorDTO Ros2ControlExecutionBackend::operator_intervention_error(
    core::ErrorCode code, std::string message, std::string phase,
    core::ExecutionState backend_state,
    std::map<std::string, std::string> details) const {
  details["requires_operator_intervention"] = "true";
  details["resync_required"] = "true";
  details["phase"] = std::move(phase);
  details["backend_state"] = core::to_string(backend_state);
  details["action_name"] = config_.action_name;
  details["stop_completion_timeout_s"] =
      format_double(config_.stop_completion_timeout_s);
  return make_error(code, std::move(message), std::move(details));
}

core::Result<void>
Ros2ControlExecutionBackend::resync_after_intervention_if_needed(
    std::string phase) {
  {
    std::unique_lock<std::mutex> lock(execution_mutex_);
    if (!resync_required_) {
      return core::Result<void>::Ok();
    }
    if (resync_in_progress_) {
      const auto timeout = std::chrono::duration<double>(
          config_.stop_completion_timeout_s + config_.wait_for_state_timeout_s);
      const bool finished = execution_cv_.wait_for(
          lock, timeout, [this]() { return !resync_in_progress_; });
      if (!finished) {
        lock.unlock();
        const auto snapshot = get_execution_snapshot();
        return core::Result<void>::Err(
            operator_intervention_error(core::ErrorCode::kResetFailed,
                                        "Cannot resync ros2_control backend: "
                                        "another resync is still in progress",
                                        phase, snapshot.state));
      }
      if (!resync_required_) {
        return core::Result<void>::Ok();
      }
    }
    if (running_) {
      lock.unlock();
      const auto snapshot = get_execution_snapshot();
      return core::Result<void>::Err(
          operator_intervention_error(core::ErrorCode::kResetFailed,
                                      "Cannot resync ros2_control backend "
                                      "while execution is still in flight",
                                      phase, snapshot.state));
    }
    resync_in_progress_ = true;
  }
  const auto resync_guard = std::shared_ptr<void>(nullptr, [this](void *) {
    {
      std::lock_guard<std::mutex> lock(execution_mutex_);
      resync_in_progress_ = false;
    }
    execution_cv_.notify_all();
  });
  (void)resync_guard;

  if (monitor_thread_.joinable() && monitor_active_.load()) {
    std::unique_lock<std::mutex> lock(execution_mutex_);
    const bool monitor_stopped = execution_cv_.wait_for(
        lock, std::chrono::duration<double>(config_.stop_completion_timeout_s),
        [this]() { return !monitor_active_.load(); });
    if (!monitor_stopped) {
      lock.unlock();
      const auto snapshot = get_execution_snapshot();
      return core::Result<void>::Err(
          operator_intervention_error(core::ErrorCode::kResetFailed,
                                      "Cannot resync ros2_control backend: "
                                      "monitor thread is still stopping",
                                      phase, snapshot.state));
    }
  }
  if (monitor_thread_.joinable()) {
    monitor_thread_.join();
  }

  if (is_commanded_sim_mode()) {
    auto reset_result =
        command_sim_initial_state(phase + "_commanded_sim_reset");
    if (!reset_result.ok()) {
      return reset_result;
    }
    {
      std::lock_guard<std::mutex> lock(execution_mutex_);
      resync_required_ = false;
      set_running_locked(false);
      terminal_ = true;
      stop_requested_ = false;
      natural_completion_stop_requested_ = false;
      pause_requested_ = false;
      goal_accepted_ = false;
      first_feedback_seen_ = false;
      result_received_ = false;
      controlled_stop_preempt_in_progress_ = false;
      active_goal_handle_.reset();
      active_goal_id_.reset();
      active_trajectory_.reset();
      controlled_stop_command_reference_.reset();
      active_trajectory_time_s_ = 0.0;
      active_runtime_velocity_scale_ = 1.0;
      stop_requested_at_.reset();
      tracking_error_since_.reset();
    }
    execution_cv_.notify_all();
    return core::Result<void>::Ok();
  }

  auto sample = wait_for_fresh_passive_sample(config_.wait_for_state_timeout_s);
  if (!sample.has_value()) {
    const auto snapshot = get_execution_snapshot();
    return core::Result<void>::Err(
        operator_intervention_error(core::ErrorCode::kStartStateMismatch,
                                    "Cannot resync ros2_control backend: no "
                                    "fresh complete controller state sample",
                                    phase, snapshot.state));
  }

  std::vector<double> positions;
  std::vector<double> velocities;
  try {
    positions = positions_for_joints(*sample, transport_joint_names());
    velocities = velocities_for_joints(*sample, transport_joint_names());
  } catch (const std::exception &exc) {
    const auto snapshot = get_execution_snapshot();
    return core::Result<void>::Err(operator_intervention_error(
        core::ErrorCode::kStartStateMismatch,
        std::string("Cannot resync ros2_control backend: observed state is "
                    "incomplete: ") +
            exc.what(),
        phase, snapshot.state));
  }

  try {
    update_scene_positions(positions);
  } catch (const std::exception &exc) {
    const auto snapshot = get_execution_snapshot();
    return core::Result<void>::Err(operator_intervention_error(
        core::ErrorCode::kResetFailed,
        std::string("Cannot resync ros2_control backend scene state: ") +
            exc.what(),
        "resync_scene_projection_failed", snapshot.state));
  }
  {
    std::lock_guard<std::mutex> lock(execution_mutex_);
    resync_required_ = false;
    set_running_locked(false);
    terminal_ = true;
    stop_requested_ = false;
    natural_completion_stop_requested_ = false;
    pause_requested_ = false;
    goal_accepted_ = false;
    first_feedback_seen_ = false;
    result_received_ = false;
    active_goal_handle_.reset();
    active_goal_id_.reset();
    active_trajectory_.reset();
    active_trajectory_time_s_ = 0.0;
    active_runtime_velocity_scale_ = 1.0;
    stop_requested_at_.reset();
    tracking_error_since_.reset();
    (void)publish_observed_snapshot(
        core::ExecutionState::kIdle, 0.0, positions, velocities,
        sample->received_timestamp_ns, std::nullopt,
        running_generation_.load(std::memory_order_acquire));
  }
  execution_cv_.notify_all();
  return core::Result<void>::Ok();
}

void Ros2ControlExecutionBackend::cancel_goal_if_known(
    const GoalHandleFJT::SharedPtr &goal_handle) noexcept {
  if (!goal_handle) {
    return;
  }
  try {
    (void)action_client_->async_cancel_goal(goal_handle);
  } catch (const std::exception &) {
    // rclcpp_action may throw if the action result already removed this goal
    // handle from the client. Stop/reset/destruction must remain idempotent.
  }
}

void Ros2ControlExecutionBackend::cancel_all_fjt_goals() noexcept {
  if (!action_client_) {
    return;
  }
  try {
    (void)action_client_->async_cancel_all_goals();
  } catch (const std::exception &) {
    // Response-timeout cleanup has no goal handle yet. The caller still fails
    // closed; this best-effort cancel prevents late accepts from running on.
  }
}

std::string Ros2ControlExecutionBackend::latest_feedback_error_summary() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (!peak_tracking_error_.has_value() ||
      peak_tracking_error_->abs_error_rad <= 0.0) {
    return {};
  }
  const auto &peak = *peak_tracking_error_;
  std::ostringstream out;
  out << std::fixed << std::setprecision(4)
      << "peak controller feedback max position error: " << peak.joint_name
      << "=" << peak.signed_error_rad << " rad"
      << " (abs=" << peak.abs_error_rad << " rad"
      << ", elapsed=" << peak.elapsed_s << " s)";
  if (peak.has_expected_position && peak.has_observed_position) {
    out << " expected=" << peak.expected_position_rad
        << " observed=" << peak.observed_position_rad;
  }
  if (peak.has_observed_velocity) {
    out << " observed_velocity=" << peak.observed_velocity_rad_s << " rad/s";
  }
  return out.str();
}

std::optional<Ros2ControlExecutionBackend::TrackingErrorPeak>
Ros2ControlExecutionBackend::update_tracking_error_peak_locked(
    const JointSample &error_sample, double elapsed_s,
    const std::vector<double> *observed_positions,
    const std::vector<double> *expected_positions,
    const std::vector<double> *observed_velocities) {
  if (error_sample.positions.empty()) {
    return peak_tracking_error_;
  }
  size_t max_index = 0;
  double max_abs_error = 0.0;
  for (size_t i = 0; i < error_sample.positions.size(); ++i) {
    const double abs_error = std::abs(error_sample.positions[i]);
    if (abs_error > max_abs_error) {
      max_abs_error = abs_error;
      max_index = i;
    }
  }
  if (max_abs_error <= 0.0 ||
      (peak_tracking_error_.has_value() &&
       max_abs_error <= peak_tracking_error_->abs_error_rad)) {
    return peak_tracking_error_;
  }

  TrackingErrorPeak peak;
  peak.joint_name = max_index < error_sample.joint_names.size()
                        ? error_sample.joint_names[max_index]
                        : "joint[" + std::to_string(max_index) + "]";
  peak.signed_error_rad = error_sample.positions[max_index];
  peak.abs_error_rad = max_abs_error;
  peak.elapsed_s = elapsed_s;
  if (expected_positions != nullptr && max_index < expected_positions->size()) {
    peak.expected_position_rad = (*expected_positions)[max_index];
    peak.has_expected_position = true;
  }
  if (observed_positions != nullptr && max_index < observed_positions->size()) {
    peak.observed_position_rad = (*observed_positions)[max_index];
    peak.has_observed_position = true;
  }
  if (observed_velocities != nullptr &&
      max_index < observed_velocities->size()) {
    peak.observed_velocity_rad_s = (*observed_velocities)[max_index];
    peak.has_observed_velocity = true;
  }
  peak_tracking_error_ = peak;
  return peak_tracking_error_;
}

void Ros2ControlExecutionBackend::fail_execution(std::string message,
                                                 core::ErrorCode code) {
  auto error = make_error(code, std::move(message));
  {
    std::lock_guard<std::mutex> lock(execution_mutex_);
    if (terminal_) {
      return;
    }
    const auto snapshot = get_execution_snapshot();
    set_running_locked(false);
    natural_completion_stop_requested_ = false;
    resync_required_ = true;
    active_goal_handle_.reset();
    active_goal_id_.reset();
    active_trajectory_.reset();
    active_trajectory_time_s_ = 0.0;
    active_runtime_velocity_scale_ = 1.0;
    const auto terminal_generation =
        running_generation_.load(std::memory_order_acquire);
    if (uses_authored_backend_state()) {
      publish_snapshot(core::ExecutionState::kFailed, snapshot.progress,
                       snapshot.joint_positions_rad, std::move(error));
    } else {
      (void)publish_observed_metadata(core::ExecutionState::kFailed,
                                      snapshot.progress, std::move(error),
                                      terminal_generation);
    }
    // `terminal_` is the completion-handle predicate. Publish the matching
    // terminal snapshot while still holding execution_mutex_ and only then
    // make completion observable, so a waiter cannot return with the prior
    // executing snapshot.
    terminal_ = true;
  }
  execution_cv_.notify_all();
}

void Ros2ControlExecutionBackend::fail_execution(
    std::string message, core::ErrorCode code, std::string reason_code,
    std::map<std::string, std::string> details) {
  details["action_name"] = config_.action_name;
  auto error = make_ros2_control_fault_error(
      code, std::move(message), std::move(reason_code), std::move(details));
  {
    std::lock_guard<std::mutex> lock(execution_mutex_);
    if (terminal_) {
      return;
    }
    const auto snapshot = get_execution_snapshot();
    set_running_locked(false);
    natural_completion_stop_requested_ = false;
    resync_required_ = true;
    active_goal_handle_.reset();
    active_goal_id_.reset();
    active_trajectory_.reset();
    active_trajectory_time_s_ = 0.0;
    active_runtime_velocity_scale_ = 1.0;
    const auto terminal_generation =
        running_generation_.load(std::memory_order_acquire);
    if (uses_authored_backend_state()) {
      publish_snapshot(core::ExecutionState::kFailed, snapshot.progress,
                       snapshot.joint_positions_rad, std::move(error));
    } else {
      (void)publish_observed_metadata(core::ExecutionState::kFailed,
                                      snapshot.progress, std::move(error),
                                      terminal_generation);
    }
    terminal_ = true;
  }
  execution_cv_.notify_all();
}

void Ros2ControlExecutionBackend::require_operator_intervention(
    std::string message, core::ErrorCode code) {
  {
    std::lock_guard<std::mutex> lock(execution_mutex_);
    if (terminal_) {
      return;
    }
    const auto snapshot = get_execution_snapshot();
    auto error = operator_intervention_error(
        code, std::move(message), "controller_cancel_timeout", snapshot.state);
    set_running_locked(false);
    natural_completion_stop_requested_ = false;
    resync_required_ = true;
    active_goal_handle_.reset();
    active_goal_id_.reset();
    active_trajectory_.reset();
    active_trajectory_time_s_ = 0.0;
    active_runtime_velocity_scale_ = 1.0;
    const auto terminal_generation =
        running_generation_.load(std::memory_order_acquire);
    if (uses_authored_backend_state()) {
      publish_snapshot(core::ExecutionState::kInterventionRequired,
                       snapshot.progress, snapshot.joint_positions_rad,
                       std::move(error));
    } else {
      (void)publish_observed_metadata(
          core::ExecutionState::kInterventionRequired, snapshot.progress,
          std::move(error), terminal_generation);
    }
    terminal_ = true;
  }
  execution_cv_.notify_all();
}

void Ros2ControlExecutionBackend::require_operator_intervention(
    std::string message, core::ErrorCode code, std::string reason_code,
    std::map<std::string, std::string> details) {
  details["action_name"] = config_.action_name;
  details["stop_completion_timeout_s"] =
      format_double(config_.stop_completion_timeout_s);
  auto error = make_ros2_control_fault_error(
      code, std::move(message), std::move(reason_code), std::move(details));
  {
    std::lock_guard<std::mutex> lock(execution_mutex_);
    if (terminal_) {
      return;
    }
    const auto snapshot = get_execution_snapshot();
    error.details["requires_operator_intervention"] = "true";
    error.details["resync_required"] = "true";
    error.details["phase"] = "controller_cancel_timeout";
    error.details["backend_state"] = core::to_string(snapshot.state);
    set_running_locked(false);
    natural_completion_stop_requested_ = false;
    resync_required_ = true;
    active_goal_handle_.reset();
    active_goal_id_.reset();
    active_trajectory_.reset();
    active_trajectory_time_s_ = 0.0;
    active_runtime_velocity_scale_ = 1.0;
    const auto terminal_generation =
        running_generation_.load(std::memory_order_acquire);
    if (uses_authored_backend_state()) {
      publish_snapshot(core::ExecutionState::kInterventionRequired,
                       snapshot.progress, snapshot.joint_positions_rad,
                       std::move(error));
    } else {
      (void)publish_observed_metadata(
          core::ExecutionState::kInterventionRequired, snapshot.progress,
          std::move(error), terminal_generation);
    }
    terminal_ = true;
  }
  execution_cv_.notify_all();
}

void Ros2ControlExecutionBackend::commit_action_result_terminal(
    const rclcpp_action::GoalUUID &goal_id, std::uint64_t execution_generation,
    bool canceled, std::optional<core::ErrorDTO> failure,
    std::chrono::steady_clock::time_point result_received_at) {
  // Admission publication and this terminal commit share the same bounded
  // linearization order as dispatch: admission -> execution -> state.
  std::lock_guard<std::mutex> admission_lock(dispatch_admission_mutex_);
  std::lock_guard<std::mutex> execution_lock(execution_mutex_);
  if (!running_ || terminal_ ||
      running_generation_.load(std::memory_order_acquire) !=
          execution_generation ||
      !active_goal_id_.has_value() || *active_goal_id_ != goal_id) {
    return;
  }

  const bool pause_requested = pause_requested_;
  const bool stop_requested = stop_requested_;
  if (!pause_requested && !stop_requested &&
      active_dispatch_authority_.has_value()) {
    auto authority =
        current_active_dispatch_authority_locked(*active_dispatch_authority_);
    if (!authority.admitted) {
      authority.details["phase"] = "terminal_result_authority";
      authority.details["admission_failure_reason"] = authority.reason_code;
      const bool binding_loss =
          authority.reason_code.rfind("controller_", 0) == 0;
      failure = make_ros2_control_fault_error(
          core::ErrorCode::kExecutionAborted,
          authority.message.empty()
              ? "Controller binding or lifecycle admission was lost "
                "before terminal commit"
              : std::move(authority.message),
          binding_loss ? ros2_control_fault_reason::kControllerBindingLost
                       : ros2_control_fault_reason::kExecutionAdmissionLost,
          std::move(authority.details));
      dispatch_authority_losses_total_.fetch_add(1, std::memory_order_relaxed);
    }
  }

  const auto current_snapshot = get_execution_snapshot();
  core::ExecutionState terminal_state = core::ExecutionState::kFailed;
  if (pause_requested) {
    terminal_state = core::ExecutionState::kPaused;
    failure.reset();
  } else if (stop_requested || canceled) {
    terminal_state = core::ExecutionState::kStopped;
    failure.reset();
  } else if (!failure.has_value()) {
    failure = make_ros2_control_fault_error(
        core::ErrorCode::kInvariantViolation,
        "FollowJointTrajectory terminal callback had no terminal outcome",
        ros2_control_fault_reason::kControllerResultFailed,
        {{"phase", "action_result_terminal_linearization"},
         {"action_name", config_.action_name}});
  }

  if (uses_authored_backend_state()) {
    (void)publish_snapshot(terminal_state, current_snapshot.progress,
                           current_snapshot.joint_positions_rad, failure, "",
                           execution_generation,
                           SnapshotPublicationAuthority::kBackendAuthored,
                           current_snapshot.joint_velocities_rad_s.empty()
                               ? nullptr
                               : &current_snapshot.joint_velocities_rad_s);
  } else {
    // This is an execution-state transition only. Keep the last measured pose,
    // source timestamp and age; never manufacture a new physical sample.
    (void)publish_observed_metadata(terminal_state, current_snapshot.progress,
                                    failure, execution_generation);
  }

  if ((pause_requested || stop_requested || canceled) &&
      stop_requested_at_.has_value()) {
    const auto latency_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            result_received_at - *stop_requested_at_)
            .count();
    record_cancel_latency_ns(latency_ns);
    record_stop_confirmation_latency_ns(latency_ns);
  }

  terminal_ = true;
  resync_required_ = terminal_state == core::ExecutionState::kFailed;
  set_running_locked(false, ProgressRetirement::kRetireImmediately,
                     pause_requested ? ResumeAuthorityRetention::kPreserve
                                     : ResumeAuthorityRetention::kDiscard);
  natural_completion_stop_requested_ = false;
  active_goal_handle_.reset();
  active_goal_id_.reset();
  active_trajectory_.reset();
  active_trajectory_time_s_ = 0.0;
  active_runtime_velocity_scale_ = 1.0;

  if (!pause_requested && !current_snapshot.joint_positions_rad.empty()) {
    project_terminal_scene_positions(
        current_snapshot.joint_positions_rad,
        uses_authored_backend_state() ? clock_.now()
                                      : current_snapshot.source_timestamp_ns);
  }
  execution_cv_.notify_all();
}

bool Ros2ControlExecutionBackend::completion_gate_matches_locked(
    const CompletionGateRequest &request) const {
  return running_ && !terminal_ &&
         running_generation_.load(std::memory_order_acquire) ==
             request.execution_generation &&
         active_goal_id_.has_value() && *active_goal_id_ == request.goal_id &&
         completion_gate_request_.has_value() &&
         completion_gate_request_->execution_generation ==
             request.execution_generation &&
         completion_gate_request_->goal_id == request.goal_id;
}

void Ros2ControlExecutionBackend::process_completion_gate(
    const CompletionGateRequest &request) {
  const auto timeout =
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::duration<double>(config_.wait_for_state_timeout_s));
  const auto deadline = request.result_received_at + timeout;
  const auto wait_slice =
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::duration<double>(
              std::max(0.001, std::min(config_.monitor_period_s, 0.01))));

  std::optional<JointSample> latest_candidate;
  std::optional<core::ErrorDTO> terminal_failure;
  while (true) {
    std::optional<DispatchAuthorityPin> dispatch_authority;
    bool terminal_intent_pending = false;
    {
      std::lock_guard<std::mutex> lock(execution_mutex_);
      if (!completion_gate_matches_locked(request)) {
        return;
      }
      terminal_intent_pending = stop_requested_ || pause_requested_;
      dispatch_authority = active_dispatch_authority_;
    }
    if (terminal_intent_pending) {
      commit_completion_gate_terminal(request, std::nullopt, std::nullopt);
      return;
    }

    if (dispatch_authority.has_value()) {
      auto authority = current_active_dispatch_authority(*dispatch_authority);
      if (!authority.admitted) {
        authority.details["phase"] = "terminal_result_authority";
        authority.details["admission_failure_reason"] = authority.reason_code;
        const bool binding_loss =
            authority.reason_code.rfind("controller_", 0) == 0;
        terminal_failure = make_ros2_control_fault_error(
            core::ErrorCode::kExecutionAborted,
            authority.message.empty()
                ? "Controller binding or lifecycle admission was lost "
                  "before terminal commit"
                : std::move(authority.message),
            binding_loss ? ros2_control_fault_reason::kControllerBindingLost
                         : ros2_control_fault_reason::kExecutionAdmissionLost,
            std::move(authority.details));
        commit_completion_gate_terminal(request, std::nullopt,
                                        std::move(terminal_failure));
        return;
      }
    }

    if (uses_authored_backend_state()) {
      commit_completion_gate_terminal(request, std::nullopt, std::nullopt);
      return;
    }

    const auto now = std::chrono::steady_clock::now();
    {
      std::lock_guard<std::mutex> state_lock(state_mutex_);
      latest_candidate = select_latest_passive_sample_after_locked(
          now, request.result_received_at);
    }
    if (latest_candidate.has_value()) {
      std::string final_error;
      if (final_state_matches(*request.trajectory, *latest_candidate,
                              final_error)) {
        commit_completion_gate_terminal(request, std::move(latest_candidate),
                                        std::nullopt);
        return;
      }
      terminal_failure = make_ros2_control_fault_error(
          core::ErrorCode::kExecutionAborted, std::move(final_error),
          ros2_control_fault_reason::kTrackingErrorExceeded,
          {{"phase", "terminal_result_state_gate"},
           {"sample_source", latest_candidate->source},
           {"sample_topic", latest_candidate->topic},
           {"final_tolerance_rad",
            format_double(config_.final_tolerance_rad)}});
    }

    if (now >= deadline) {
      if (!terminal_failure.has_value()) {
        terminal_failure = make_ros2_control_fault_error(
            core::ErrorCode::kExecutionAborted,
            "No fresh controller or joint-state sample was received after "
            "the successful FollowJointTrajectory result",
            ros2_control_fault_reason::kStaleControllerState,
            {{"phase", "terminal_result_state_gate"},
             {"wait_for_state_timeout_s",
              format_double(config_.wait_for_state_timeout_s)},
             {"final_tolerance_rad",
              format_double(config_.final_tolerance_rad)}});
      }
      commit_completion_gate_terminal(request, std::move(latest_candidate),
                                      std::move(terminal_failure));
      return;
    }

    // State callbacks wake this wait immediately. The bounded slice also
    // observes Stop/Pause/Reset promptly without making those paths depend on
    // the state callback group.
    std::unique_lock<std::mutex> state_lock(state_mutex_);
    state_cv_.wait_for(state_lock, std::min(wait_slice, deadline - now));
  }
}

void Ros2ControlExecutionBackend::commit_completion_gate_terminal(
    const CompletionGateRequest &request,
    std::optional<JointSample> verified_observed_sample,
    std::optional<core::ErrorDTO> failure) {
  std::lock_guard<std::mutex> admission_lock(dispatch_admission_mutex_);
  std::lock_guard<std::mutex> lock(execution_mutex_);
  if (!completion_gate_matches_locked(request)) {
    return;
  }

  const bool pause_requested = pause_requested_;
  const bool stop_requested = stop_requested_;
  if (!pause_requested && !stop_requested &&
      active_dispatch_authority_.has_value()) {
    auto authority =
        current_active_dispatch_authority_locked(*active_dispatch_authority_);
    if (!authority.admitted) {
      authority.details["phase"] = "terminal_result_authority";
      authority.details["admission_failure_reason"] = authority.reason_code;
      const bool binding_loss =
          authority.reason_code.rfind("controller_", 0) == 0;
      failure = make_ros2_control_fault_error(
          core::ErrorCode::kExecutionAborted,
          authority.message.empty()
              ? "Controller binding or lifecycle admission was lost "
                "before terminal commit"
              : std::move(authority.message),
          binding_loss ? ros2_control_fault_reason::kControllerBindingLost
                       : ros2_control_fault_reason::kExecutionAdmissionLost,
          std::move(authority.details));
    }
  }
  if (failure.has_value() && failure->details.contains("phase") &&
      failure->details.at("phase") == "terminal_result_authority") {
    dispatch_authority_losses_total_.fetch_add(1, std::memory_order_relaxed);
  }
  const auto current_snapshot = get_execution_snapshot();
  core::ExecutionState terminal_state = core::ExecutionState::kCompleted;
  double terminal_progress = 1.0;
  if (pause_requested) {
    terminal_state = core::ExecutionState::kPaused;
    terminal_progress = current_snapshot.progress;
    failure.reset();
  } else if (stop_requested) {
    terminal_state = core::ExecutionState::kStopped;
    terminal_progress = current_snapshot.progress;
    failure.reset();
  } else if (failure.has_value()) {
    terminal_state = core::ExecutionState::kFailed;
    terminal_progress = current_snapshot.progress;
  }

  std::vector<double> terminal_positions = current_snapshot.joint_positions_rad;
  std::vector<double> terminal_velocities =
      current_snapshot.joint_velocities_rad_s;
  std::int64_t terminal_source_timestamp_ns =
      current_snapshot.source_timestamp_ns;

  if (uses_authored_backend_state()) {
    if (terminal_state == core::ExecutionState::kCompleted &&
        request.trajectory && !request.trajectory->waypoints.empty()) {
      terminal_positions = request.trajectory->waypoints.back().positions_rad;
      terminal_velocities =
          request.trajectory->waypoints.back().velocities_rad_s;
    }
    (void)publish_snapshot(
        terminal_state, terminal_progress, terminal_positions,
        std::move(failure), "", request.execution_generation,
        SnapshotPublicationAuthority::kBackendAuthored,
        terminal_velocities.empty() ? nullptr : &terminal_velocities);
    terminal_source_timestamp_ns = clock_.now();
  } else {
    // execution_mutex_ -> state_mutex_ is the declared lock order. Holding
    // both here makes the chosen physical sample, terminal snapshot, and
    // subsequent scene submission one exact generation-bound handoff. A
    // concurrently arriving newer sample cannot be overwritten by an older
    // verified one.
    std::lock_guard<std::mutex> state_lock(state_mutex_);
    const auto now = std::chrono::steady_clock::now();
    const JointSample *newest = nullptr;
    if (pause_requested || stop_requested) {
      newest = select_latest_passive_sample_ptr_locked(now);
    } else {
      const auto selected = select_latest_passive_sample_after_locked(
          now, request.result_received_at);
      if (selected.has_value()) {
        verified_observed_sample = std::move(selected);
      }
      if (verified_observed_sample.has_value()) {
        newest = &*verified_observed_sample;
      }
    }

    if (newest != nullptr) {
      try {
        auto positions = positions_for_joints(*newest, transport_joint_names());
        auto velocities =
            velocities_for_joints(*newest, transport_joint_names());
        const bool measurement_gate_failure =
            failure.has_value() && failure->details.contains("phase") &&
            failure->details.at("phase") == "terminal_result_state_gate";
        if (!pause_requested && !stop_requested &&
            (!failure.has_value() || measurement_gate_failure)) {
          std::string final_error;
          if (final_state_matches(*request.trajectory, *newest, final_error)) {
            // A fresh in-tolerance physical sample supersedes an earlier
            // timeout/mismatch found by the bounded wait.
            if (!failure.has_value() || measurement_gate_failure) {
              failure.reset();
              terminal_state = core::ExecutionState::kCompleted;
              terminal_progress = 1.0;
            }
          } else {
            failure = make_ros2_control_fault_error(
                core::ErrorCode::kExecutionAborted, std::move(final_error),
                ros2_control_fault_reason::kTrackingErrorExceeded,
                {{"phase", "terminal_result_state_gate"},
                 {"sample_source", newest->source},
                 {"sample_topic", newest->topic},
                 {"final_tolerance_rad",
                  format_double(config_.final_tolerance_rad)}});
            terminal_state = core::ExecutionState::kFailed;
            terminal_progress = current_snapshot.progress;
          }
        }
        if (snapshot_.state_source != "observed_ros2" ||
            snapshot_.source_timestamp_ns <= newest->received_timestamp_ns) {
          terminal_positions = std::move(positions);
          terminal_velocities = std::move(velocities);
          terminal_source_timestamp_ns = newest->received_timestamp_ns;
          snapshot_.joint_positions_rad = terminal_positions;
          snapshot_.joint_velocities_rad_s = terminal_velocities;
          snapshot_.state_source = "observed_ros2";
          snapshot_.source_timestamp_ns = terminal_source_timestamp_ns;
        } else {
          terminal_positions = snapshot_.joint_positions_rad;
          terminal_velocities = snapshot_.joint_velocities_rad_s;
          terminal_source_timestamp_ns = snapshot_.source_timestamp_ns;
        }
      } catch (const std::exception &exc) {
        if (!pause_requested && !stop_requested) {
          failure = make_ros2_control_fault_error(
              core::ErrorCode::kExecutionAborted,
              std::string("Terminal physical-state sample was incomplete: ") +
                  exc.what(),
              ros2_control_fault_reason::kTrackingSampleInvalid,
              {{"phase", "terminal_result_state_gate"},
               {"sample_source", newest->source},
               {"sample_topic", newest->topic},
               {"final_tolerance_rad",
                format_double(config_.final_tolerance_rad)}});
          terminal_state = core::ExecutionState::kFailed;
          terminal_progress = current_snapshot.progress;
        }
      }
    }
    snapshot_.timestamp_ns = clock_.now();
    snapshot_.sequence_number++;
    snapshot_.state = terminal_state;
    snapshot_.progress = clamp_progress(terminal_progress);
    snapshot_.source_age_ns =
        snapshot_.timestamp_ns >= snapshot_.source_timestamp_ns
            ? snapshot_.timestamp_ns - snapshot_.source_timestamp_ns
            : 0;
    snapshot_.error = failure;
  }

  if (!terminal_positions.empty()) {
    project_terminal_scene_positions(terminal_positions,
                                     terminal_source_timestamp_ns);
  }
  if (terminal_state == core::ExecutionState::kCompleted) {
    const auto terminal_progress_ns = clock_.now();
    const auto terminal_trajectory_time_s =
        request.trajectory && !request.trajectory->waypoints.empty()
            ? request.trajectory->waypoints.back().time_from_start_s
            : active_trajectory_time_s_;
    (void)execution_progress_publication_->publish_terminal(
        request.execution_generation,
        static_cast<std::int64_t>(std::max(0.0, terminal_trajectory_time_s) *
                                  1e9),
        terminal_progress_ns, terminal_progress_ns);
  }

  terminal_ = true;
  resync_required_ = terminal_state == core::ExecutionState::kFailed;
  set_running_locked(false,
                     terminal_state == core::ExecutionState::kCompleted
                         ? ProgressRetirement::kPreserveTerminalHandoff
                         : ProgressRetirement::kRetireImmediately,
                     pause_requested ? ResumeAuthorityRetention::kPreserve
                                     : ResumeAuthorityRetention::kDiscard);
  natural_completion_stop_requested_ = false;
  active_goal_handle_.reset();
  active_goal_id_.reset();
  active_trajectory_.reset();
  active_trajectory_time_s_ = 0.0;
  active_runtime_velocity_scale_ = 1.0;

  if ((pause_requested || stop_requested) && stop_requested_at_.has_value()) {
    const auto latency_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - *stop_requested_at_)
            .count();
    record_cancel_latency_ns(latency_ns);
    record_stop_confirmation_latency_ns(latency_ns);
  }
  execution_cv_.notify_all();
}

void Ros2ControlExecutionBackend::complete_execution(
    const JointSample *verified_observed_sample) {
  std::vector<double> final_positions =
      get_execution_snapshot().joint_positions_rad;
  std::vector<double> final_velocities;
  std::int64_t terminal_scene_source_ns = clock_.now();
  if (!uses_authored_backend_state() && verified_observed_sample != nullptr) {
    try {
      final_positions = positions_for_joints(*verified_observed_sample,
                                             transport_joint_names());
      final_velocities = velocities_for_joints(*verified_observed_sample,
                                               transport_joint_names());
      terminal_scene_source_ns =
          verified_observed_sample->received_timestamp_ns;
    } catch (const std::exception &) {
      // The caller verified completeness before committing the terminal
      // transition. Keep the last snapshot only for the test-only seam.
    }
  }
  const auto generation = running_generation_.load(std::memory_order_acquire);
  if (uses_authored_backend_state()) {
    (void)publish_snapshot(
        core::ExecutionState::kCompleted, 1.0, final_positions, std::nullopt,
        "", generation, SnapshotPublicationAuthority::kBackendAuthored,
        final_velocities.empty() ? nullptr : &final_velocities);
  } else if (verified_observed_sample != nullptr) {
    (void)publish_observed_snapshot(
        core::ExecutionState::kCompleted, 1.0, final_positions,
        final_velocities, terminal_scene_source_ns, std::nullopt, generation);
  } else {
    (void)publish_observed_metadata(core::ExecutionState::kCompleted, 1.0,
                                    std::nullopt, generation);
  }
  execution_cv_.notify_all();
  if (!final_positions.empty()) {
    project_terminal_scene_positions(final_positions, terminal_scene_source_ns);
  }
}

bool Ros2ControlExecutionBackend::start_state_matches(
    const core::TrajectoryDTO &trajectory, const JointSample &sample,
    std::string &error, std::map<std::string, std::string> *details) const {
  if (trajectory.waypoints.empty()) {
    return true;
  }
  std::vector<double> observed;
  try {
    observed = positions_for_joints(sample, transport_joint_names());
  } catch (const std::exception &e) {
    error = e.what();
    return false;
  }
  const auto &start = trajectory.waypoints.front().positions_rad;
  const size_t count = std::min(observed.size(), start.size());
  double max_error = 0.0;
  size_t max_index = 0;
  for (size_t i = 0; i < count; ++i) {
    const double joint_error = std::abs(observed[i] - start[i]);
    if (joint_error > max_error) {
      max_error = joint_error;
      max_index = i;
    }
  }
  if (max_error > config_.start_tolerance_rad) {
    error = "Start state mismatch: max joint error " +
            std::to_string(max_error) + " rad exceeds tolerance " +
            std::to_string(config_.start_tolerance_rad);
    if (details != nullptr) {
      (*details)["max_joint_error_rad"] = format_double(max_error);
      (*details)["start_tolerance_rad"] =
          format_double(config_.start_tolerance_rad);
      (*details)["observed_position_rad"] =
          max_index < observed.size() ? format_double(observed[max_index]) : "";
      (*details)["expected_position_rad"] =
          max_index < start.size() ? format_double(start[max_index]) : "";
      (*details)["joint_name"] =
          max_index < trajectory.joint_names.size()
              ? trajectory.joint_names[max_index]
              : "joint[" + std::to_string(max_index) + "]";
    }
    return false;
  }
  return true;
}

bool Ros2ControlExecutionBackend::final_state_matches(
    const core::TrajectoryDTO &trajectory, const JointSample &sample,
    std::string &error) const {
  if (trajectory.waypoints.empty()) {
    return true;
  }
  std::vector<double> observed;
  try {
    observed = positions_for_joints(sample, transport_joint_names());
  } catch (const std::exception &e) {
    error = e.what();
    return false;
  }
  const auto &goal = trajectory.waypoints.back().positions_rad;
  const double max_error = max_abs_error(observed, goal);
  if (max_error > config_.final_tolerance_rad) {
    error = "Final state mismatch: max joint error " +
            std::to_string(max_error) + " rad exceeds tolerance " +
            std::to_string(config_.final_tolerance_rad);
    return false;
  }
  return true;
}

double Ros2ControlExecutionBackend::max_abs_error(
    const std::vector<double> &lhs, const std::vector<double> &rhs) const {
  const size_t count = std::min(lhs.size(), rhs.size());
  double max_error = 0.0;
  for (size_t i = 0; i < count; ++i) {
    max_error = std::max(max_error, std::abs(lhs[i] - rhs[i]));
  }
  return max_error;
}

core::ExecutionHandle Ros2ControlExecutionBackend::execute_trajectory(
    const core::TrajectoryDTO &trajectory) {
  auto resync_result = resync_after_intervention_if_needed("execute_resync");
  if (!resync_result.ok()) {
    const auto snapshot = get_execution_snapshot();
    if (uses_authored_backend_state()) {
      (void)publish_snapshot(core::ExecutionState::kInterventionRequired,
                             snapshot.progress, snapshot.joint_positions_rad,
                             resync_result.error());
    } else {
      std::lock_guard<std::mutex> lock(execution_mutex_);
      (void)publish_observed_metadata(
          core::ExecutionState::kInterventionRequired, snapshot.progress,
          resync_result.error(),
          running_generation_.load(std::memory_order_acquire));
    }
    return core::ExecutionHandle{[](int64_t) { return true; },
                                 [this]() { return get_execution_snapshot(); }};
  }

  std::uint64_t dispatch_generation = 0;
  {
    std::lock_guard<std::mutex> lock(execution_mutex_);
    if (running_) {
      auto rejected = get_execution_snapshot();
      rejected.timestamp_ns = clock_.now();
      ++rejected.sequence_number;
      rejected.error = make_error(core::ErrorCode::kInvalidRequest,
                                  "Execution already in flight");
      return core::ExecutionHandle{[](int64_t) { return true; },
                                   [rejected]() { return rejected; }};
    } else {
      set_running_locked(true);
      authored_active_last_publication_ns_.store(0, std::memory_order_release);
      terminal_ = false;
      stop_requested_ = false;
      natural_completion_stop_requested_ = false;
      pause_requested_ = false;
      goal_accepted_ = false;
      first_feedback_seen_ = false;
      result_received_ = false;
      last_action_result_code_.reset();
      active_goal_handle_.reset();
      active_goal_id_.reset();
      active_trajectory_.reset();
      active_trajectory_time_s_ = 0.0;
      active_runtime_velocity_scale_ = 1.0;
      stop_requested_at_.reset();
      tracking_error_since_.reset();
      peak_tracking_error_.reset();
      execution_started_at_ = std::chrono::steady_clock::now();
      const double initial_runtime_scale =
          config_.enable_jtc_speed_scaling_input
              ? runtime_velocity_scale_.load(std::memory_order_acquire)
              : 1.0;
      applied_runtime_velocity_scale_.store(initial_runtime_scale,
                                            std::memory_order_release);
      active_trajectory_ =
          std::make_shared<const core::TrajectoryDTO>(trajectory);
      active_trajectory_time_s_ = 0.0;
      active_runtime_velocity_scale_ = initial_runtime_scale;
      dispatch_generation = running_generation_.load(std::memory_order_acquire);
    }
  }

  std::vector<double> start_positions;
  std::vector<double> start_velocities;
  std::int64_t start_source_timestamp_ns = clock_.now();
  std::int64_t progress_source_timestamp_ns = clock_.now();
  if (uses_authored_backend_state()) {
    if (trajectory.waypoints.empty()) {
      fail_execution(
          "Trajectory has no waypoints", core::ErrorCode::kInvalidRequest,
          ros2_control_fault_reason::kEmptyTrajectory,
          {{"waypoint_count", "0"},
           {"joint_count", std::to_string(trajectory.joint_names.size())}});
      return core::ExecutionHandle{
          [](int64_t) { return true; },
          [this]() { return get_execution_snapshot(); }};
    }
    start_positions = trajectory.waypoints.front().positions_rad;
  } else {
    auto start_sample =
        wait_for_fresh_passive_sample(config_.wait_for_state_timeout_s);
    if (!start_sample.has_value()) {
      fail_execution(
          "No fresh complete controller state sample before dispatch",
          core::ErrorCode::kExecutionRejected,
          ros2_control_fault_reason::kStaleControllerState,
          {{"wait_for_state_timeout_s",
            format_double(config_.wait_for_state_timeout_s)},
           {"joint_state_topic", config_.joint_state_topic},
           {"controller_state_topic", config_.controller_state_topic},
           {"required_joint_count",
            std::to_string(config_.required_joints.size())}});
      return core::ExecutionHandle{
          [this](int64_t timeout_ns) {
            std::unique_lock<std::mutex> lock(execution_mutex_);
            if (timeout_ns > 0) {
              return execution_cv_.wait_for(
                  lock, std::chrono::nanoseconds(timeout_ns),
                  [this]() { return terminal_; });
            }
            execution_cv_.wait(lock, [this]() { return terminal_; });
            return true;
          },
          [this]() { return get_execution_snapshot(); }};
    }

    std::string start_error;
    std::map<std::string, std::string> start_details;
    if (!start_state_matches(trajectory, *start_sample, start_error,
                             &start_details)) {
      fail_execution(start_error, core::ErrorCode::kStartStateMismatch,
                     ros2_control_fault_reason::kStartStateMismatch,
                     std::move(start_details));
      return core::ExecutionHandle{
          [](int64_t) { return true; },
          [this]() { return get_execution_snapshot(); }};
    }
    start_positions =
        positions_for_joints(*start_sample, transport_joint_names());
    start_velocities =
        velocities_for_joints(*start_sample, transport_joint_names());
    start_source_timestamp_ns = start_sample->received_timestamp_ns;
    progress_source_timestamp_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            start_sample->received_at.time_since_epoch())
            .count();
  }

  const bool start_snapshot_published =
      uses_authored_backend_state()
          ? publish_snapshot(
                core::ExecutionState::kExecuting, 0.0, start_positions,
                std::nullopt, "", dispatch_generation,
                SnapshotPublicationAuthority::kBackendAuthored,
                start_velocities.empty() ? nullptr : &start_velocities)
          : publish_observed_snapshot(core::ExecutionState::kExecuting, 0.0,
                                      start_positions, start_velocities,
                                      start_source_timestamp_ns, std::nullopt,
                                      dispatch_generation);
  if (start_snapshot_published) {
    (void)schedule_active_scene_projection(start_positions,
                                           uses_authored_backend_state()
                                               ? clock_.now()
                                               : start_source_timestamp_ns);
  }

  if (!wait_for_action_server_until(action_client_,
                                    config_.wait_for_action_server_timeout_s)) {
    fail_execution("FollowJointTrajectory action server unavailable: " +
                       config_.action_name,
                   core::ErrorCode::kExecutionRejected,
                   ros2_control_fault_reason::kActionServerUnavailable,
                   {{"wait_for_action_server_timeout_s",
                     format_double(config_.wait_for_action_server_timeout_s)}});
    return core::ExecutionHandle{[](int64_t) { return true; },
                                 [this]() { return get_execution_snapshot(); }};
  }

  FollowJointTrajectory::Goal goal;
  goal.trajectory.joint_names = transport_joint_names();
  if (config_.disable_controller_path_tolerances) {
    disable_fjt_controller_tolerances(goal, transport_joint_names());
  }
  publish_runtime_velocity_scale(
      runtime_velocity_scale_.load(std::memory_order_acquire));
  for (const auto &waypoint : trajectory.waypoints) {
    trajectory_msgs::msg::JointTrajectoryPoint point;
    point.positions = waypoint.positions_rad;
    point.velocities = waypoint.velocities_rad_s;
    point.accelerations = waypoint.accelerations_rad_s2;
    point.time_from_start = duration_from_seconds(waypoint.time_from_start_s);
    goal.trajectory.points.push_back(std::move(point));
  }

  rclcpp_action::Client<FollowJointTrajectory>::SendGoalOptions options;
  const auto record_goal_acceptance =
      [this, dispatch_generation](GoalHandleFJT::SharedPtr goal_handle) {
        if (!goal_handle) {
          return;
        }
        const auto accepted_at = std::chrono::steady_clock::now();
        bool cancel_late_goal = false;
        {
          std::lock_guard<std::mutex> lock(execution_mutex_);
          if (!running_ || terminal_ ||
              running_generation_.load(std::memory_order_acquire) !=
                  dispatch_generation) {
            cancel_late_goal = true;
          } else {
            const auto goal_id = goal_handle->get_goal_id();
            if (active_goal_id_.has_value() && *active_goal_id_ != goal_id) {
              cancel_late_goal = true;
            } else {
              if (!goal_accepted_) {
                active_goal_accepted_at_ = accepted_at;
              }
              active_goal_handle_ = goal_handle;
              active_goal_id_ = goal_id;
              active_action_feedback_authority_generation_.store(
                  dispatch_generation, std::memory_order_release);
              goal_accepted_ = true;
            }
          }
        }
        if (cancel_late_goal) {
          cancel_goal_if_known(goal_handle);
        }
      };
  options.goal_response_callback = record_goal_acceptance;
  options.feedback_callback =
      [this](GoalHandleFJT::SharedPtr goal_handle,
             const std::shared_ptr<const FollowJointTrajectory::Feedback>
                 feedback) { on_action_feedback(goal_handle, feedback); };
  options.result_callback =
      [this, dispatch_generation](const GoalHandleFJT::WrappedResult &result) {
        on_action_result(result, dispatch_generation);
      };

  std::shared_future<GoalHandleFJT::SharedPtr> goal_handle_future;
  {
    // This is the Phase 2.7A linearization point. Controller-manager refresh
    // and lifecycle-admission publishers serialize with this bounded action
    // enqueue. There are no service calls, waits, model expansion, or YAML
    // parsing in this section.
    std::lock_guard<std::mutex> admission_lock(dispatch_admission_mutex_);
    const auto binding =
        controller_binding_snapshot_.load(std::memory_order_acquire);
    const auto admission =
        execution_admission_snapshot_.load(std::memory_order_acquire);
    auto decision = evaluate_dispatch_admission(
        config_.controller_binding.has_value() ? &*config_.controller_binding
                                               : nullptr,
        binding.get(), admission.get(),
        config_.require_execution_admission_snapshot, clock_.now(),
        config_.action_name, transport_joint_names());
    if (!decision.admitted || !decision.pin.has_value()) {
      const bool binding_rejection =
          decision.reason_code.rfind("controller_", 0) == 0;
      (binding_rejection ? controller_binding_dispatch_rejections_total_
                         : execution_admission_dispatch_rejections_total_)
          .fetch_add(1, std::memory_order_relaxed);
      decision.details["admission_failure_reason"] = decision.reason_code;
      fail_execution(
          decision.message.empty()
              ? "Controller binding or lifecycle admission rejected dispatch"
              : std::move(decision.message),
          core::ErrorCode::kExecutionRejected,
          binding_rejection
              ? ros2_control_fault_reason::kControllerBindingRejected
              : ros2_control_fault_reason::kExecutionAdmissionRejected,
          std::move(decision.details));
      return core::ExecutionHandle{
          [](int64_t) { return true; },
          [this]() { return get_execution_snapshot(); }};
    }
    decision.pin->execution_backend_component_epoch =
        execution_backend_component_epoch_;
    if ((config_.controller_binding.has_value() ||
         config_.require_execution_admission_snapshot) &&
        (!planning_environment_authority_.has_value() ||
         !same_execution_environment(*planning_environment_authority_,
                                     *decision.pin))) {
      execution_admission_dispatch_rejections_total_.fetch_add(
          1, std::memory_order_relaxed);
      fail_execution(
          "Execution environment changed between planning and dispatch",
          core::ErrorCode::kExecutionRejected,
          ros2_control_fault_reason::kExecutionAdmissionRejected,
          {{"phase", "planning_to_dispatch_environment_pin"},
           {"reason", planning_environment_authority_.has_value()
                          ? "execution_environment_epoch_changed"
                          : "planning_environment_pin_missing"}});
      return core::ExecutionHandle{
          [](int64_t) { return true; },
          [this]() { return get_execution_snapshot(); }};
    }
    if (!install_dispatch_authority_pin(*decision.pin, dispatch_generation)) {
      fail_execution("Execution generation changed before controller dispatch",
                     core::ErrorCode::kExecutionRejected,
                     ros2_control_fault_reason::kControllerGoalDispatchFailed,
                     {{"phase", "dispatch_authority_pin"}});
      return core::ExecutionHandle{
          [](int64_t) { return true; },
          [this]() { return get_execution_snapshot(); }};
    }
    planning_environment_authority_.reset();
    try {
      goal_handle_future = action_client_->async_send_goal(goal, options);
    } catch (const std::exception &exc) {
      fail_execution(
          std::string("FollowJointTrajectory goal dispatch failed: ") +
              exc.what(),
          core::ErrorCode::kExecutionRejected,
          ros2_control_fault_reason::kControllerGoalDispatchFailed,
          {{"exception", exc.what()}});
      return core::ExecutionHandle{
          [](int64_t) { return true; },
          [this]() { return get_execution_snapshot(); }};
    }
  }
  if (goal_handle_future.wait_for(std::chrono::duration<double>(
          config_.wait_for_action_server_timeout_s)) !=
      std::future_status::ready) {
    cancel_all_fjt_goals();
    fail_execution(
        "Timed out waiting for FollowJointTrajectory goal response: " +
            config_.action_name,
        core::ErrorCode::kExecutionRejected,
        ros2_control_fault_reason::kControllerGoalResponseTimeout,
        {{"wait_for_action_server_timeout_s",
          format_double(config_.wait_for_action_server_timeout_s)}});
    return core::ExecutionHandle{[](int64_t) { return true; },
                                 [this]() { return get_execution_snapshot(); }};
  }

  GoalHandleFJT::SharedPtr goal_handle;
  try {
    goal_handle = goal_handle_future.get();
  } catch (const std::exception &exc) {
    fail_execution(std::string("FollowJointTrajectory goal dispatch failed: ") +
                       exc.what(),
                   core::ErrorCode::kExecutionRejected,
                   ros2_control_fault_reason::kControllerGoalDispatchFailed,
                   {{"exception", exc.what()}});
    return core::ExecutionHandle{[](int64_t) { return true; },
                                 [this]() { return get_execution_snapshot(); }};
  }
  if (!goal_handle) {
    // Only name the gate when one is configured: non-gated deployments
    // (real controllers) must not get a false gate attribution.
    const bool gate_configured = !config_.command_gate_topic.empty();
    const bool gate_closed = gate_configured && !command_gate_enabled_.load(
                                                    std::memory_order_acquire);
    std::map<std::string, std::string> details;
    if (gate_configured) {
      details["command_gate_enabled"] = gate_closed ? "false" : "true";
    }
    fail_execution(
        gate_closed ? "FollowJointTrajectory goal rejected (command gate "
                      "closed: simulator reset incomplete or failed — run "
                      "simulation reset)"
                    : "FollowJointTrajectory goal rejected",
        core::ErrorCode::kExecutionRejected,
        ros2_control_fault_reason::kControllerGoalRejected, std::move(details));
    return core::ExecutionHandle{[](int64_t) { return true; },
                                 [this]() { return get_execution_snapshot(); }};
  }

  record_goal_acceptance(goal_handle);
  {
    std::optional<DispatchAuthorityPin> goal_response_authority;
    {
      std::lock_guard<std::mutex> lock(execution_mutex_);
      if (!running_ || terminal_ ||
          running_generation_.load(std::memory_order_acquire) !=
              dispatch_generation) {
        return core::ExecutionHandle{
            [](int64_t) { return true; },
            [this]() { return get_execution_snapshot(); }, dispatch_generation};
      }
      goal_response_authority = active_dispatch_authority_;
    }
    if (goal_response_authority.has_value()) {
      auto authority =
          current_active_dispatch_authority(*goal_response_authority);
      if (!authority.admitted) {
        authority.details["phase"] = "goal_response_authority";
        fail_active_dispatch_authority(std::move(authority));
        return core::ExecutionHandle{
            [](int64_t) { return true; },
            [this]() { return get_execution_snapshot(); }, dispatch_generation};
      }
    }
  }
  {
    std::lock_guard<std::mutex> lock(execution_mutex_);
    if (!running_ || terminal_ ||
        running_generation_.load(std::memory_order_acquire) !=
            dispatch_generation) {
      return core::ExecutionHandle{
          [](int64_t) { return true; },
          [this]() { return get_execution_snapshot(); }, dispatch_generation};
    }
  }
  if (is_commanded_sim_mode()) {
    set_command_gate(true);
  }

  const auto progress_received_ns = clock_.now();
  const auto initial_progress = execution_progress_publication_->publish(
      dispatch_generation, 0, progress_source_timestamp_ns,
      progress_received_ns);
  if (!initial_progress.published) {
    cancel_goal_if_known(goal_handle);
    fail_execution("Execution progress authority could not be armed",
                   core::ErrorCode::kExecutionRejected,
                   "execution_progress_publication_rejected",
                   {{"reason", initial_progress.reason}});
    return core::ExecutionHandle{[](int64_t) { return true; },
                                 [this]() { return get_execution_snapshot(); },
                                 0};
  }

  if (monitor_thread_.joinable() && monitor_active_.load()) {
    fail_execution(
        "Previous ros2_control monitor thread is still shutting down",
        core::ErrorCode::kExecutionRejected,
        ros2_control_fault_reason::kMonitorThreadStillShuttingDown, {});
    return core::ExecutionHandle{[](int64_t) { return true; },
                                 [this]() { return get_execution_snapshot(); }};
  }
  if (monitor_thread_.joinable()) {
    monitor_thread_.join();
  }
  monitor_thread_ = std::thread(
      [this, trajectory_copy = core::TrajectoryDTO(trajectory)]() mutable {
        monitor_worker(std::move(trajectory_copy));
      });

  return core::ExecutionHandle{
      [this](int64_t timeout_ns) {
        std::unique_lock<std::mutex> lock(execution_mutex_);
        if (timeout_ns > 0) {
          return execution_cv_.wait_for(lock,
                                        std::chrono::nanoseconds(timeout_ns),
                                        [this]() { return terminal_; });
        }
        execution_cv_.wait(lock, [this]() { return terminal_; });
        return true;
      },
      [this]() { return get_execution_snapshot(); }, dispatch_generation};
}

void Ros2ControlExecutionBackend::monitor_worker(
    core::TrajectoryDTO trajectory) {
  monitor_active_.store(true);
  const auto monitor_generation =
      running_generation_.load(std::memory_order_acquire);
  const auto monitor_guard = std::shared_ptr<void>(nullptr, [this](void *) {
    {
      std::lock_guard<std::mutex> lock(execution_mutex_);
      monitor_active_.store(false);
    }
    execution_cv_.notify_all();
  });
  (void)monitor_guard;

  const double total_duration_s =
      trajectory.waypoints.empty()
          ? 0.0
          : trajectory.waypoints.back().time_from_start_s;
  const auto trajectory_sampling_valid =
      core::validate_trajectory_for_sampling(trajectory);
  if (!trajectory_sampling_valid.ok() && node_) {
    RCLCPP_WARN(
        node_->get_logger(),
        "trajectory_sampler validation rejected Ros2ControlExecutionBackend "
        "monitor trajectory: %s (waypoints=%zu)",
        trajectory_sampling_valid.error().code.c_str(),
        trajectory.waypoints.size());
  }
  core::TrajectorySampleResult predicted_sample;
  auto last_monitor_tick = std::chrono::steady_clock::now();
  double trajectory_time_s = 0.0;
  double measured_progress_time_s = 0.0;
  auto last_progress_projection_at =
      std::chrono::steady_clock::time_point::min();
  double applied_runtime_scale =
      applied_runtime_velocity_scale_.load(std::memory_order_acquire);
  while (true) {
    const auto now = std::chrono::steady_clock::now();
    std::optional<CompletionGateRequest> completion_gate;
    bool cancel_pending = false;
    bool cancel_timeout = false;
    bool controlled_stop_replacement_active = false;
    bool natural_completion_stop_requested = false;
    bool waiting_for_first_feedback = false;
    bool first_feedback_timeout = false;
    std::optional<DispatchAuthorityPin> dispatch_authority;
    {
      std::lock_guard<std::mutex> lock(execution_mutex_);
      if (terminal_ || !running_) {
        return;
      }
      if (completion_gate_request_.has_value() &&
          completion_gate_matches_locked(*completion_gate_request_)) {
        completion_gate = completion_gate_request_;
      }
      cancel_pending = stop_requested_ || pause_requested_;
      controlled_stop_replacement_active = controlled_stop_preempt_in_progress_;
      natural_completion_stop_requested = natural_completion_stop_requested_;
      cancel_timeout = cancel_pending && !controlled_stop_replacement_active &&
                       stop_requested_at_.has_value() &&
                       seconds_since(*stop_requested_at_, now) >=
                           config_.stop_completion_timeout_s;
      waiting_for_first_feedback = !first_feedback_seen_;
      first_feedback_timeout = waiting_for_first_feedback &&
                               seconds_since(execution_started_at_, now) >=
                                   config_.wait_for_state_timeout_s;
      dispatch_authority = active_dispatch_authority_;
    }
    if (completion_gate.has_value()) {
      process_completion_gate(*completion_gate);
      return;
    }
    if (dispatch_authority.has_value()) {
      auto authority = current_active_dispatch_authority(*dispatch_authority);
      if (!authority.admitted) {
        authority.details["phase"] = "active_goal_authority";
        fail_active_dispatch_authority(std::move(authority));
        return;
      }
    }
    if (cancel_timeout) {
      require_operator_intervention(
          "Timed out waiting for FollowJointTrajectory cancel confirmation",
          core::ErrorCode::kStopFailed,
          ros2_control_fault_reason::kControllerCancelTimeout,
          {{"stop_completion_timeout_s",
            format_double(config_.stop_completion_timeout_s)}});
      return;
    }
    if (cancel_pending) {
      if (!publish_controlled_stop_command_reference_snapshot(
              now, monitor_generation)) {
        const auto snapshot = get_execution_snapshot();
        if (!publish_observed_cancel_stop_snapshot(trajectory.joint_names,
                                                   snapshot.progress, now,
                                                   monitor_generation)) {
          if (uses_authored_backend_state()) {
            (void)publish_snapshot(core::ExecutionState::kStopping,
                                   snapshot.progress,
                                   snapshot.joint_positions_rad, std::nullopt,
                                   "", monitor_generation);
          } else {
            (void)publish_observed_metadata(core::ExecutionState::kStopping,
                                            snapshot.progress, std::nullopt,
                                            monitor_generation);
          }
        }
      }
      // Plain JTC cancel/pause waits for the old goal to terminate and is
      // guarded by `stop_completion_timeout_s` above. Managed controlled-stop
      // replacements also set stop_requested_ so the UI sees Stopping, but
      // their terminal deadline is enforced by execute_controlled_stop(wait_s).
      // Do not apply the cancel-confirmation timeout to those timed replacement
      // goals.
      {
        std::unique_lock<std::mutex> lock(execution_mutex_);
        execution_cv_.wait_for(
            lock, std::chrono::duration<double>(config_.monitor_period_s),
            [this]() { return terminal_ || !running_; });
      }
      continue;
    }
    if (first_feedback_timeout) {
      fail_execution(
          "Timed out waiting for first FollowJointTrajectory feedback",
          core::ErrorCode::kExecutionRejected,
          ros2_control_fault_reason::kControllerFeedbackTimeout,
          {{"wait_for_state_timeout_s",
            format_double(config_.wait_for_state_timeout_s)}});
      return;
    }
    if (waiting_for_first_feedback) {
      last_monitor_tick = now;
      std::vector<double> snapshot_positions;
      std::vector<double> snapshot_velocities;
      std::int64_t snapshot_source_timestamp_ns = 0;
      bool measured_snapshot_available = false;
      if (uses_authored_backend_state() && !trajectory.waypoints.empty()) {
        snapshot_positions = trajectory.waypoints.front().positions_rad;
        snapshot_velocities = trajectory.waypoints.front().velocities_rad_s;
      } else {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        auto sample = select_latest_sample_locked(now);
        if (sample.has_value()) {
          try {
            snapshot_positions =
                positions_for_joints(*sample, transport_joint_names());
            snapshot_velocities =
                velocities_for_joints(*sample, transport_joint_names());
            snapshot_source_timestamp_ns = sample->received_timestamp_ns;
            measured_snapshot_available = true;
          } catch (const std::exception &) {
            snapshot_positions.clear();
            snapshot_velocities.clear();
          }
        }
      }
      if (uses_authored_backend_state() && snapshot_positions.empty() &&
          !trajectory.waypoints.empty()) {
        snapshot_positions = trajectory.waypoints.front().positions_rad;
      }
      const auto state = natural_completion_stop_requested
                             ? core::ExecutionState::kStopping
                             : core::ExecutionState::kExecuting;
      bool snapshot_published = false;
      if (uses_authored_backend_state()) {
        snapshot_published = publish_snapshot(
            state, 0.0, snapshot_positions, std::nullopt, "",
            monitor_generation, SnapshotPublicationAuthority::kBackendAuthored,
            snapshot_velocities.empty() ? nullptr : &snapshot_velocities);
      } else if (measured_snapshot_available) {
        snapshot_published = publish_observed_snapshot(
            state, 0.0, snapshot_positions, snapshot_velocities,
            snapshot_source_timestamp_ns, std::nullopt, monitor_generation);
      } else {
        // No physical sample is available yet. Preserve the last measured
        // pose and its aging provenance; the trajectory's first waypoint is
        // intent, not real-robot state.
        snapshot_published = publish_observed_metadata(state, 0.0, std::nullopt,
                                                       monitor_generation);
      }
      if (snapshot_published && !snapshot_positions.empty() &&
          (uses_authored_backend_state() || measured_snapshot_available)) {
        (void)schedule_active_scene_projection(
            snapshot_positions, uses_authored_backend_state()
                                    ? clock_.now()
                                    : snapshot_source_timestamp_ns);
      }
      // Audit 6C (finding 2.5): wake on first feedback / terminal /
      // stop-pause intent instead of sleeping out the period
      // (on_action_feedback notifies execution_cv_ on the first-feedback
      // transition).
      {
        std::unique_lock<std::mutex> lock(execution_mutex_);
        execution_cv_.wait_for(
            lock, std::chrono::duration<double>(config_.monitor_period_s),
            [this]() {
              return first_feedback_seen_ || terminal_ || !running_ ||
                     stop_requested_ || pause_requested_ ||
                     completion_gate_request_.has_value();
            });
      }
      continue;
    }
    const double dt_s = std::max(
        0.0, std::chrono::duration<double>(now - last_monitor_tick).count());
    last_monitor_tick = now;
    const double target_scale =
        config_.enable_jtc_speed_scaling_input
            ? clamp_progress(
                  runtime_velocity_scale_.load(std::memory_order_acquire))
            : 1.0;
    const double max_delta = kRuntimeScaleSlewPerSecond * dt_s;
    if (target_scale > applied_runtime_scale + max_delta) {
      applied_runtime_scale += max_delta;
    } else if (target_scale < applied_runtime_scale - max_delta) {
      applied_runtime_scale -= max_delta;
    } else {
      applied_runtime_scale = target_scale;
    }
    applied_runtime_velocity_scale_.store(applied_runtime_scale,
                                          std::memory_order_release);
    trajectory_time_s = std::min(
        total_duration_s, trajectory_time_s + dt_s * applied_runtime_scale);
    {
      std::lock_guard<std::mutex> lock(execution_mutex_);
      active_trajectory_time_s_ = trajectory_time_s;
      active_runtime_velocity_scale_ = applied_runtime_scale;
    }
    if (uses_authored_backend_state()) {
      const auto now_ns = clock_.now();
      (void)execution_progress_publication_->publish(
          monitor_generation,
          static_cast<std::int64_t>(std::max(0.0, trajectory_time_s) * 1e9),
          now_ns, now_ns);
    }
    const double progress =
        total_duration_s <= 1e-9
            ? 1.0
            : clamp_progress(trajectory_time_s / total_duration_s);
    std::vector<double> predicted_positions;
    if (trajectory_sampling_valid.ok()) {
      auto sample = core::sample_trajectory_prevalidated(
          trajectory, trajectory_time_s, predicted_sample);
      if (sample.ok()) {
        predicted_positions = predicted_sample.positions_rad;
      } else {
        // Audit 6F: this runs on the 100 Hz monitor tick — a persistently
        // failing sampler must emit one bounded structured warning per
        // window, not one line per tick (and through the ROS logger, not a
        // bare std::cerr that structured consumers never see).
        RCLCPP_WARN_THROTTLE(
            node_->get_logger(), *node_->get_clock(), 5000,
            "trajectory_sampler error in Ros2ControlExecutionBackend "
            "monitor predicted-state: %s (time_s=%.3f, waypoints=%zu)",
            sample.error().code.c_str(), trajectory_time_s,
            trajectory.waypoints.size());
      }
    }
    std::vector<double> snapshot_positions = uses_authored_backend_state()
                                                 ? predicted_positions
                                                 : std::vector<double>{};
    std::vector<double> snapshot_velocities;
    std::int64_t snapshot_source_timestamp_ns = 0;
    bool measured_snapshot_available = false;
    std::optional<std::string> tracking_failure;
    std::string tracking_failure_reason_code =
        ros2_control_fault_reason::kTrackingErrorExceeded;
    std::map<std::string, std::string> tracking_failure_details;
    // Audit 6C (findings 2.14 + 2.15): copy the sample under a short
    // state_mutex_ hold, run joint mapping and the tracking-error math
    // lock-free (joint-state callbacks contend on the same mutex at
    // 100 Hz), then re-take the lock briefly only for the peak/streak
    // bookkeeping it actually guards. The expected positions reuse this
    // tick's predicted sample (same trajectory, same time) instead of
    // re-sampling the trajectory inside the critical section.
    std::optional<JointSample> sample;
    {
      std::lock_guard<std::mutex> state_lock(state_mutex_);
      sample = select_latest_sample_locked(now);
    }
    if (sample.has_value()) {
      try {
        auto observed = positions_for_joints(*sample, transport_joint_names());
        if (!uses_authored_backend_state() &&
            (last_progress_projection_at ==
                 std::chrono::steady_clock::time_point::min() ||
             seconds_since(last_progress_projection_at, now) >=
                 config_.execution_progress_projection_period_s)) {
          core::TrajectoryProgressProjectionConfig projection_config;
          projection_config.max_rms_distance_rad =
              config_.execution_progress_projection_max_rms_distance_rad;
          projection_config.ambiguity_distance_epsilon_rad =
              config_.execution_progress_projection_ambiguity_epsilon_rad;
          projection_config.ambiguity_time_separation_s =
              config_.execution_progress_projection_ambiguity_time_s;
          projection_config.regression_tolerance_s =
              config_.execution_progress_projection_regression_tolerance_s;
          const auto projected = core::project_trajectory_progress(
              trajectory, observed, measured_progress_time_s,
              projection_config);
          last_progress_projection_at = now;
          if (projected.status ==
              core::TrajectoryProgressProjectionStatus::kProjected) {
            measured_progress_time_s = projected.trajectory_time_s;
            const auto source_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    sample->received_at.time_since_epoch())
                    .count();
            (void)execution_progress_publication_->publish(
                monitor_generation,
                static_cast<std::int64_t>(measured_progress_time_s * 1e9),
                source_ns, clock_.now());
          } else {
            // A wrong cursor is less safe than no cursor. The lookahead
            // authority observes this invalidation and stops fail-closed; the
            // controller callback/JTC path remains untouched.
            (void)execution_progress_publication_->invalidate_generation(
                monitor_generation);
          }
        }
        if (!uses_authored_backend_state()) {
          snapshot_positions = observed;
          snapshot_velocities =
              velocities_for_joints(*sample, transport_joint_names());
          snapshot_source_timestamp_ns = sample->received_timestamp_ns;
          measured_snapshot_available = true;
        }
        const std::vector<double> &expected = predicted_positions;
        // Guard: an empty expected vector means either the trajectory was
        // empty (no predicted state to compare against) or the sampler
        // refused the input (logged where it was sampled). max_abs_error
        // compares only min(lhs.size(), rhs.size()) and would silently
        // return 0.0 on a zero-length overlap, which would falsely declare
        // tracking-clean. Skip the diagnostic computation for this tick
        // when the predicted state is unavailable.
        if (!expected.empty()) {
          const double error = max_abs_error(observed, expected);
          JointSample error_sample;
          error_sample.joint_names = trajectory.joint_names;
          error_sample.positions.reserve(observed.size());
          for (size_t i = 0; i < std::min(observed.size(), expected.size());
               ++i) {
            error_sample.positions.push_back(observed[i] - expected[i]);
          }
          error_sample.velocities = sample->velocities;
          error_sample.received_at = now;
          error_sample.source = sample->source + "_tracking_error";
          error_sample.topic = sample->topic;
          {
            std::lock_guard<std::mutex> state_lock(state_mutex_);
            update_tracking_error_peak_locked(error_sample, trajectory_time_s,
                                              &observed, &expected,
                                              &sample->velocities);
            if (error > config_.tracking_error_tolerance_rad &&
                config_.tracking_error_timeout_s > 0.0) {
              if (!tracking_error_since_.has_value()) {
                tracking_error_since_ = now;
              }
              if (seconds_since(*tracking_error_since_, now) >=
                  config_.tracking_error_timeout_s) {
                tracking_failure =
                    "Controller tracking error exceeded tolerance: " +
                    std::to_string(error) + " rad";
                tracking_failure_details["tracking_error_rad"] =
                    format_double(error);
                tracking_failure_details["tracking_error_tolerance_rad"] =
                    format_double(config_.tracking_error_tolerance_rad);
                tracking_failure_details["tracking_error_timeout_s"] =
                    format_double(config_.tracking_error_timeout_s);
                tracking_failure_details["trajectory_elapsed_s"] =
                    format_double(trajectory_time_s);
                if (peak_tracking_error_.has_value()) {
                  const auto &peak = *peak_tracking_error_;
                  tracking_failure_details["joint_name"] = peak.joint_name;
                  tracking_failure_details["signed_error_rad"] =
                      format_double(peak.signed_error_rad);
                  tracking_failure_details["peak_tracking_error_rad"] =
                      format_double(peak.abs_error_rad);
                  tracking_failure_details["peak_elapsed_s"] =
                      format_double(peak.elapsed_s);
                  if (peak.has_expected_position) {
                    tracking_failure_details["expected_position_rad"] =
                        format_double(peak.expected_position_rad);
                  }
                  if (peak.has_observed_position) {
                    tracking_failure_details["observed_position_rad"] =
                        format_double(peak.observed_position_rad);
                  }
                  if (peak.has_observed_velocity) {
                    tracking_failure_details["observed_velocity_rad_s"] =
                        format_double(peak.observed_velocity_rad_s);
                  }
                }
              }
            } else {
              tracking_error_since_.reset();
            }
          }
        }
      } catch (const std::exception &e) {
        tracking_failure =
            "Controller tracking sample was malformed or incomplete";
        tracking_failure_reason_code =
            ros2_control_fault_reason::kTrackingSampleInvalid;
        tracking_failure_details["reason_code"] =
            ros2_control_fault_reason::kTrackingSampleInvalid;
        tracking_failure_details["sample_source"] = sample->source;
        tracking_failure_details["sample_topic"] = sample->topic;
        tracking_failure_details["sample_joint_count"] =
            std::to_string(sample->joint_names.size());
        tracking_failure_details["sample_position_count"] =
            std::to_string(sample->positions.size());
        tracking_failure_details["sample_velocity_count"] =
            std::to_string(sample->velocities.size());
        tracking_failure_details["required_joint_count"] =
            std::to_string(trajectory.joint_names.size());
        tracking_failure_details["exception"] = e.what();
      }
    }
    if (tracking_failure.has_value()) {
      const auto feedback_error = latest_feedback_error_summary();
      if (!feedback_error.empty()) {
        *tracking_failure += "; " + feedback_error;
      }
      GoalHandleFJT::SharedPtr handle_to_cancel;
      {
        std::lock_guard<std::mutex> lock(execution_mutex_);
        handle_to_cancel = active_goal_handle_;
      }
      cancel_goal_if_known(handle_to_cancel);
      fail_execution(*tracking_failure, core::ErrorCode::kExecutionAborted,
                     tracking_failure_reason_code,
                     std::move(tracking_failure_details));
      return;
    }
    const auto state = natural_completion_stop_requested
                           ? core::ExecutionState::kStopping
                           : core::ExecutionState::kExecuting;
    bool snapshot_published = false;
    if (uses_authored_backend_state()) {
      snapshot_published = publish_snapshot(
          state, progress, snapshot_positions, std::nullopt, "",
          monitor_generation, SnapshotPublicationAuthority::kBackendAuthored,
          predicted_positions.empty() ? nullptr
                                      : &predicted_sample.velocities_rad_s);
    } else if (measured_snapshot_available) {
      snapshot_published = publish_observed_snapshot(
          state, progress, snapshot_positions, snapshot_velocities,
          snapshot_source_timestamp_ns, std::nullopt, monitor_generation);
    } else {
      snapshot_published = publish_observed_metadata(
          state, progress, std::nullopt, monitor_generation);
    }
    if (snapshot_published && !snapshot_positions.empty() &&
        (uses_authored_backend_state() || measured_snapshot_available)) {
      (void)schedule_active_scene_projection(
          snapshot_positions, uses_authored_backend_state()
                                  ? clock_.now()
                                  : snapshot_source_timestamp_ns);
    }
    // Audit 6C (finding 2.5): the period still PACES the tick (waking on
    // every 100 Hz sample would burn CPU for nothing), but terminal and
    // stop/pause intent wake the worker immediately instead of waiting out
    // the remainder of the slice.
    {
      std::unique_lock<std::mutex> lock(execution_mutex_);
      execution_cv_.wait_for(
          lock, std::chrono::duration<double>(config_.monitor_period_s),
          [this]() {
            return terminal_ || !running_ || stop_requested_ ||
                   pause_requested_ || completion_gate_request_.has_value();
          });
    }
  }
}

core::StopResultDTO Ros2ControlExecutionBackend::stop() {
  return stop_if_running_generation(std::nullopt);
}

void Ros2ControlExecutionBackend::observe_stop_qualification(
    const detail::Ros2ControlStopQualificationEvent event,
    const std::string_view detail_text) const {
  auto *const probe =
      stop_qualification_probe_.load(std::memory_order_acquire);
  if (probe != nullptr) {
    probe->observe(event, detail_text);
  }
}

core::StopResultDTO Ros2ControlExecutionBackend::stop_if_running_generation(
    std::optional<std::uint64_t> expected_running_generation,
    const bool observe_software_hard_issue) {
  core::ExecutionSnapshot snapshot;
  GoalHandleFJT::SharedPtr goal_to_cancel;
  bool cancel_requested = false;
  bool resync_required = false;
  std::uint64_t publication_generation = 0;
  {
    std::lock_guard<std::mutex> lock(execution_mutex_);
    if (expected_running_generation.has_value() &&
        running_generation_.load(std::memory_order_acquire) !=
            *expected_running_generation) {
      return core::StopResultDTO{
          false,
          make_error(core::ErrorCode::kExecutionRejected,
                     "Prepared Stop no longer owns the active execution",
                     {{"reason", "prepared_goal_execution_generation_stale"}})};
    }
    resync_required = resync_required_;
    if (resync_required_) {
      // Read the presentation snapshot only after releasing the execution
      // mutex below; this branch issues no command.
    } else if (!running_ && terminal_) {
      return core::StopResultDTO{false,
                                 make_error(core::ErrorCode::kInvalidRequest,
                                            "No execution in flight")};
    } else {
      stop_requested_ = true;
      natural_completion_stop_requested_ = false;
      pause_requested_ = false;
      stop_requested_at_ = std::chrono::steady_clock::now();
      cancel_requested = static_cast<bool>(active_goal_handle_) ||
                         controlled_stop_preempt_in_progress_;
      if (active_goal_handle_) {
        goal_to_cancel = active_goal_handle_;
      } else if (!controlled_stop_preempt_in_progress_) {
        set_running_locked(false);
        terminal_ = true;
      }
      publication_generation =
          running_generation_.load(std::memory_order_acquire);
    }
  }
  if (resync_required) {
    snapshot = get_execution_snapshot();
    return core::StopResultDTO{
        false,
        operator_intervention_error(core::ErrorCode::kStopFailed,
                                    "ros2_control execution backend requires "
                                    "observed-state resync before reuse",
                                    "stop_resync_required", snapshot.state)};
  }
  execution_cv_.notify_all();
  if (cancel_requested && !goal_to_cancel) {
    std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
    goal_to_cancel = managed_fjt_goal_;
    if (goal_to_cancel) {
      forced_managed_stop_execution_generation_ = publication_generation;
      forced_managed_stop_generation_ = managed_fjt_generation_;
    }
  }
  if (cancel_requested) {
    cancel_goal_if_known(goal_to_cancel);
  }
  if (observe_software_hard_issue &&
      (!cancel_requested || static_cast<bool>(goal_to_cancel))) {
    observe_stop_qualification(
        detail::Ros2ControlStopQualificationEvent::kSoftwareHardIssueAccepted,
        "generation_fenced_stop_accepted");
  }
  snapshot = get_execution_snapshot();
  const auto state = cancel_requested ? core::ExecutionState::kStopping
                                      : core::ExecutionState::kStopped;
  if (uses_authored_backend_state()) {
    (void)publish_snapshot(state, snapshot.progress,
                           snapshot.joint_positions_rad, std::nullopt, "",
                           publication_generation);
  } else {
    (void)publish_observed_metadata(state, snapshot.progress, std::nullopt,
                                    publication_generation);
  }
  cancel_all_gripper_commands();
  execution_cv_.notify_all();
  return core::StopResultDTO{true, std::nullopt};
}

// Controlled-stop capability + result surface.
//
// The ros2_control adapter always exposes `kSpringBackHold` in v1.
// `kSoftOnPath` additionally requires a computed replacement trajectory with
// path-deviation evidence; `kHardDeceleration` requires
// `decelerate_on_cancel=true` plus complete per-joint controller limits.
// Advertising only modes supported by the current configuration keeps the
// kernel's pre-dispatch capability gate honest.

core::MotionInterruptStopCapabilityDTO
Ros2ControlExecutionBackend::compute_motion_interrupt_stop_capability(
    const Ros2ControlExecutionBackendConfig &config) {
  return compute_ros2_control_motion_interrupt_stop_capability(config);
}

core::MotionInterruptStopCapabilityDTO
Ros2ControlExecutionBackend::get_motion_interrupt_stop_capability() const {
  return compute_motion_interrupt_stop_capability(config_);
}

core::ParticipantStopStageResultDTO
Ros2ControlExecutionBackend::TranslateStopStage(
    const core::ResolvedParticipantStopStageDTO &stage,
    const core::RemainingStopBudgetDTO &budget,
    const std::stop_token cancellation) {
  const auto started_at = clock_.now();
  if (cancellation.stop_requested()) {
    return unresolved_stop_stage_result(
        stage, core::ParticipantStopStageOutcomeDTO::kSupersededNoEffect,
        "stop_stage_superseded_before_dispatch",
        "Stop stage was cancelled before ros2_control dispatch",
        "stop_stage_superseded_before_dispatch");
  }

  const auto failure_before_effect = [&](std::string reason,
                                         std::string message) {
    return unresolved_stop_stage_result(
        stage, core::ParticipantStopStageOutcomeDTO::kFailed,
        std::string(core::kErrorMotionInterruptBackendStopFailed),
        std::move(message), std::move(reason));
  };
  const auto remaining_phase = [&](const core::StopBudgetPhaseId &phase_id) {
    const auto found = std::find_if(
        budget.phases.begin(), budget.phases.end(), [&](const auto &phase) {
          return phase.phase_id == phase_id;
        });
    return found == budget.phases.end()
               ? std::optional<std::int64_t>{}
               : std::optional(found->remaining_deadline_offset_ns);
  };

  if (stage.stage_index >= stage.participant_stop_plan.stage_commands.size() ||
      budget.stop_id != stage.stop_id ||
      budget.stop_generation != stage.stop_generation ||
      budget.remaining_outer_duration_ns <= 0) {
    return failure_before_effect(
        "resolved_stop_stage_budget_invalid",
        "ros2_control Stop stage has no valid remaining Kernel budget");
  }

  const auto retained_binding =
      prepared_participant_binding_.load(std::memory_order_acquire);
  if (!retained_binding || !retained_binding->value().control_path.has_value()) {
    return failure_before_effect(
        "resolved_stop_stage_binding_unavailable",
        "ros2_control Stop stage has no retained execution binding");
  }
  const auto &binding = retained_binding->value();
  const auto &path = *binding.control_path;
  const auto &command =
      stage.participant_stop_plan.stage_commands[stage.stage_index]
          .resolved_stage;
  const auto command_remaining =
      remaining_phase(command.containment_command.command_phase_id);
  std::vector<core::ResourceId> exact_resources;
  exact_resources.reserve(binding.resources.size());
  for (const auto &resource : binding.resources) {
    exact_resources.push_back(resource.resource_id);
  }
  std::sort(exact_resources.begin(), exact_resources.end());

  if (stage.controllable_component_id != binding.controllable_component_id ||
      stage.command_target_id != binding.command_target_id ||
      stage.capability_id != binding.capability_id ||
      stage.resources != exact_resources ||
      stage.control_domain_id != path.control_domain_id ||
      stage.control_domain_revision != path.control_domain_revision ||
      stage.control_binding_id != path.control_binding_id ||
      stage.control_binding_revision != path.control_binding_revision ||
      stage.controller_topology_id != path.controller_topology_id ||
      stage.topology_revision != path.topology_revision ||
      stage.control_route_id != path.control_route_id ||
      stage.active_control_configuration_id !=
          path.active_control_configuration_id ||
      stage.active_control_configuration_generation !=
          path.active_control_configuration_generation ||
      stage.stop_evidence_policy_id != path.stop_evidence_policy_id ||
      stage.stop_evidence_policy_revision !=
          path.stop_evidence_policy_revision ||
      stage.capability_provider_binding_id.has_value() ||
      stage.capability_provider_binding_revision.has_value() ||
      stage.provider_runtime_epoch != path.provider_runtime_epoch ||
      command.command_target.provider_instance_id !=
          path.provider_instance_id ||
      command.command_target.provider_manifest_id !=
          path.provider_manifest_id ||
      command.command_target.provider_manifest_revision !=
          path.provider_manifest_revision ||
      command.command_target.runtime_unit_id !=
          path.provider_runtime_unit_id ||
      command.command_target.provider_runtime_epoch !=
          path.provider_runtime_epoch ||
      !command_remaining.has_value() || *command_remaining <= 0) {
    return failure_before_effect(
        "resolved_stop_stage_binding_mismatch",
        "ros2_control Stop stage crosses its exact retained execution "
        "binding");
  }

  std::optional<PreparedGoalRecord> prepared_goal;
  {
    std::lock_guard<std::mutex> lock(prepared_execution_mutex_);
    const auto found = prepared_goals_.find(stage.controller_goal_id);
    if (found != prepared_goals_.end()) {
      prepared_goal = found->second;
    }
  }
  if (!prepared_goal.has_value() ||
      prepared_goal->generation != stage.controller_goal_generation) {
    return failure_before_effect(
        "resolved_stop_stage_dispatch_membership_unavailable",
        "ros2_control cannot prove this Stop stage belongs to an admitted "
        "prepared goal generation");
  }

  const bool exact_participant =
      prepared_goal->operation_id == stage.operation_id &&
      prepared_goal->operation_revision == stage.operation_revision &&
      prepared_goal->participant_id == stage.participant_id &&
      prepared_goal->controllable_component_id ==
          stage.controllable_component_id &&
      prepared_goal->command_target_id == stage.command_target_id &&
      prepared_goal->capability_id == stage.capability_id &&
      prepared_goal->control_domain_id == stage.control_domain_id &&
      prepared_goal->control_domain_revision == stage.control_domain_revision &&
      prepared_goal->control_binding_id == stage.control_binding_id &&
      prepared_goal->control_binding_revision == stage.control_binding_revision &&
      prepared_goal->controller_topology_id == stage.controller_topology_id &&
      prepared_goal->topology_revision == stage.topology_revision &&
      prepared_goal->control_route_id == stage.control_route_id &&
      prepared_goal->active_control_configuration_id ==
          stage.active_control_configuration_id &&
      prepared_goal->active_control_configuration_generation ==
          stage.active_control_configuration_generation &&
      prepared_goal->provider_runtime_epoch ==
          stage.provider_runtime_epoch &&
      prepared_goal->resources == stage.resources &&
      prepared_goal->stop_evidence_policy_id ==
          stage.stop_evidence_policy_id &&
      prepared_goal->stop_evidence_policy_revision ==
          stage.stop_evidence_policy_revision &&
      std::find(prepared_goal->stop_plans.begin(),
                prepared_goal->stop_plans.end(),
                stage.participant_stop_plan) !=
          prepared_goal->stop_plans.end();
  if (!exact_participant) {
    return failure_before_effect(
        "resolved_stop_stage_dispatch_membership_mismatch",
        "ros2_control Stop stage differs from its admitted prepared goal");
  }

  const bool controller_goal_cancel =
      command.containment_command.command_kind ==
      core::StopContainmentCommandKind::kControllerGoalCancel;
  const bool software_hard =
      command.containment_command.command_kind ==
          core::StopContainmentCommandKind::kRegistered &&
      command.containment_command.registered_command_kind_id.has_value() &&
      command.containment_command.registered_command_kind_id->value() ==
          "manyforge.stop.software_hard";
  if (!controller_goal_cancel && !software_hard) {
    return failure_before_effect(
        "resolved_stop_command_not_supported",
        "ros2_control exact Stop port supports controller-goal cancel and "
        "its retained software-hard command");
  }

  const auto dispatch_generation = prepared_goal->handle.execution_generation;
  const auto current_generation =
      running_generation_.load(std::memory_order_acquire);
  const bool execution_running =
      running_flag_.load(std::memory_order_acquire);
  if (dispatch_generation == 0U ||
      current_generation != dispatch_generation || !execution_running) {
    auto failure = failure_before_effect(
        "prepared_goal_not_active",
        "ros2_control prepared goal no longer owns the active execution");
    if (failure.error.has_value()) {
      failure.error->details["prepared_execution_generation"] =
          std::to_string(dispatch_generation);
      failure.error->details["current_execution_generation"] =
          std::to_string(current_generation);
      failure.error->details["current_execution_activity"] =
          execution_running ? "running" : "terminal_or_inactive";
    }
    return failure;
  }
  if (cancellation.stop_requested()) {
    return unresolved_stop_stage_result(
        stage, core::ParticipantStopStageOutcomeDTO::kSupersededNoEffect,
        "stop_stage_superseded_before_dispatch",
        "Stop stage was cancelled before ros2_control dispatch",
        "stop_stage_superseded_before_dispatch");
  }

  const auto stopped =
      stop_if_running_generation(dispatch_generation, software_hard);
  if (!stopped.accepted) {
    const auto reason = stopped.error.has_value() &&
                                stopped.error->details.contains("reason")
                            ? stopped.error->details.at("reason")
                            : "stop_command_not_dispatched";
    return failure_before_effect(
        reason, stopped.error.has_value()
                    ? stopped.error->message
                    : "ros2_control Stop command was not accepted");
  }
  core::ParticipantStopStageResultDTO result{
      stage,
      core::StopStageAttemptDispositionDTO::kDispatched,
      core::ParticipantStopStageOutcomeDTO::kFailed,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      0,
      {},
      std::nullopt};
  const auto fail_after_dispatch = [&](std::string reason,
                                       std::string message) {
    result.receiver_elapsed_duration_ns =
        std::max<std::int64_t>(0, clock_.now() - started_at);
    result.error = core::ErrorDTO{
        std::string(core::kErrorMotionInterruptBackendStopFailed),
        std::move(message), {{"reason", std::move(reason)}}};
    result.evidence_items.clear();
    result.evidence_items.reserve(command.evidence_requirements.size());
    for (const auto &requirement : command.evidence_requirements) {
      result.evidence_items.push_back(core::StopEvidenceItemDTO{
          requirement.requirement.evidence_item_id,
          requirement.requirement.evidence_kind,
          core::StopEvidenceItemStateDTO::kMissing,
          requirement.resolved_source,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          result.error});
    }
    return result;
  };

  if (command.evidence_requirements.empty()) {
    return fail_after_dispatch(
        "stop_evidence_requirements_missing",
        "ros2_control Stop stage has no retained evidence contract");
  }
  std::int64_t evidence_remaining_ns = budget.remaining_outer_duration_ns;
  std::int64_t minimum_dwell_ns = 0;
  for (const auto &requirement : command.evidence_requirements) {
    const auto phase = remaining_phase(requirement.requirement.phase_id);
    if (!phase.has_value() || *phase <= 0) {
      return fail_after_dispatch(
          "stop_evidence_phase_expired",
          "ros2_control Stop evidence phase is already expired");
    }
    evidence_remaining_ns = std::min(evidence_remaining_ns, *phase);
    minimum_dwell_ns = std::max(
        minimum_dwell_ns,
        requirement.requirement.observation_semantics.minimum_dwell_ns
            .value_or(0));
  }
  if (evidence_remaining_ns <= 0) {
    return fail_after_dispatch(
        "stop_evidence_phase_expired",
        "ros2_control Stop evidence has no remaining receiver budget");
  }

  const auto evidence_started_at = std::chrono::steady_clock::now();
  const auto evidence_deadline =
      evidence_started_at + std::chrono::nanoseconds(evidence_remaining_ns);
  bool terminal = false;
  while (!terminal && std::chrono::steady_clock::now() < evidence_deadline) {
    if (cancellation.stop_requested()) {
      return fail_after_dispatch(
          "stop_stage_cancelled_after_dispatch",
          "ros2_control Stop evidence wait was superseded");
    }
    const auto remaining = evidence_deadline - std::chrono::steady_clock::now();
    const auto slice = std::min(
        remaining,
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::milliseconds(10)));
    const auto slice_ns = std::max<std::int64_t>(
        1, std::chrono::duration_cast<std::chrono::nanoseconds>(slice).count());
    terminal = prepared_goal->handle.wait_for_completion(slice_ns);
  }
  if (!terminal) {
    return fail_after_dispatch(
        "stop_evidence_phase_expired",
        "ros2_control execution did not reach a terminal state in budget");
  }
  const auto terminal_snapshot = prepared_goal->handle.get_snapshot();
  if (terminal_snapshot.state != core::ExecutionState::kStopped) {
    return fail_after_dispatch(
        "stop_goal_terminal_state_mismatch",
        "ros2_control goal did not terminate through the requested Stop");
  }

  if (controller_goal_cancel) {
    const auto now_ns = clock_.now();
    if (terminal_snapshot.sequence_number == 0U ||
        terminal_snapshot.timestamp_ns > now_ns) {
      return fail_after_dispatch(
          "stop_evidence_source_unavailable",
          "ros2_control goal-terminal observation has no valid local "
          "sequence or timestamp");
    }
    const auto source_age_ns =
        std::max<std::int64_t>(0, now_ns - terminal_snapshot.timestamp_ns);
    result.evidence_items.reserve(command.evidence_requirements.size());
    for (const auto &requirement : command.evidence_requirements) {
      if (requirement.requirement.evidence_kind !=
              core::StopEvidenceRequirementKind::kControllerGoalTerminal ||
          requirement.resolved_source.source_kind !=
              core::StopEvidenceSourceKind::kControllerGoalStatus ||
          requirement.resolved_source.runtime_target.provider_runtime_epoch !=
              path.provider_runtime_epoch ||
          source_age_ns >
              requirement.requirement.source_requirement
                  .freshness_budget_ns) {
        return fail_after_dispatch(
            "stop_evidence_source_mismatch",
            "ros2_control goal-terminal evidence differs from the retained "
            "source contract");
      }
      result.evidence_items.push_back(core::StopEvidenceItemDTO{
          requirement.requirement.evidence_item_id,
          requirement.requirement.evidence_kind,
          core::StopEvidenceItemStateDTO::kSatisfied,
          requirement.resolved_source,
          path.provider_runtime_epoch,
          terminal_snapshot.sequence_number,
          source_age_ns,
          core::JsonValueDTO{true},
          std::nullopt});
    }
    result.controller_acknowledgement_sequence =
        terminal_snapshot.sequence_number;
    result.source_epoch = path.provider_runtime_epoch;
    result.source_sequence = terminal_snapshot.sequence_number;
  } else {
    struct RestObservation final {
      std::uint64_t sequence{};
      std::int64_t received_timestamp_ns{};
    };
    std::optional<RestObservation> rest;
    auto rest_started_at = std::chrono::steady_clock::time_point::min();
    bool rest_tracking = false;
    while (std::chrono::steady_clock::now() < evidence_deadline) {
      if (cancellation.stop_requested()) {
        return fail_after_dispatch(
            "stop_stage_cancelled_after_dispatch",
            "ros2_control Stop rest-evidence wait was superseded");
      }
      const auto now_ns = clock_.now();
      auto observed = observed_state_publication_->acquire(now_ns);
      bool exact_rest = false;
      if (observed && observed.complete() && observed.velocities_complete() &&
          std::equal(observed.joint_names().begin(),
                     observed.joint_names().end(),
                     config_.required_joints.begin(),
                     config_.required_joints.end()) &&
          observed.received_timestamp_ns() <= now_ns) {
        const auto expected_profile =
            path.state_authority_class == core::StateAuthorityClass::kMeasured
                ? core::ObservedRobotStateAuthorityProfile::kMeasured
                : core::ObservedRobotStateAuthorityProfile::
                      kCommandedSimulation;
        exact_rest = observed.authority_profile() == expected_profile &&
                     std::all_of(observed.velocities_rad_s().begin(),
                                 observed.velocities_rad_s().end(),
                                 [](double velocity) {
                                   return std::isfinite(velocity) &&
                                          std::abs(velocity) <=
                                              core::kDefaultSettleVelocityThresholdRadS;
                                 });
        if (exact_rest) {
          if (!rest_tracking) {
            rest_started_at = std::chrono::steady_clock::now();
            rest_tracking = true;
          }
          rest = RestObservation{observed.sequence(),
                                 observed.received_timestamp_ns()};
        }
      }
      if (!exact_rest) {
        rest_tracking = false;
        rest.reset();
      } else if (rest_tracking &&
                 std::chrono::steady_clock::now() - rest_started_at >=
                     std::chrono::nanoseconds(minimum_dwell_ns)) {
        break;
      }
      const auto remaining = evidence_deadline -
                             std::chrono::steady_clock::now();
      if (remaining <= std::chrono::steady_clock::duration::zero()) {
        break;
      }
      std::this_thread::sleep_for(std::min(
          remaining,
          std::chrono::duration_cast<std::chrono::steady_clock::duration>(
              std::chrono::milliseconds(2))));
    }
    if (!rest.has_value() || rest->sequence == 0U || !rest_tracking ||
        std::chrono::steady_clock::now() - rest_started_at <
            std::chrono::nanoseconds(minimum_dwell_ns)) {
      return fail_after_dispatch(
          "stop_evidence_not_at_rest",
          "ros2_control addressed state did not prove bounded rest");
    }

    const auto now_ns = clock_.now();
    const auto source_age_ns = std::max<std::int64_t>(
        0, now_ns - rest->received_timestamp_ns);
    result.evidence_items.reserve(command.evidence_requirements.size());
    for (const auto &requirement : command.evidence_requirements) {
      if (requirement.requirement.evidence_kind !=
              core::StopEvidenceRequirementKind::kMotionRest ||
          requirement.resolved_source.source_kind !=
              core::StopEvidenceSourceKind::kBindingStateAuthority ||
          requirement.resolved_source.runtime_target.provider_runtime_epoch !=
              path.provider_runtime_epoch ||
          requirement.resolved_source.state_authority_id !=
              std::optional(path.state_authority_id) ||
          requirement.resolved_source.state_authority_class !=
              std::optional(path.state_authority_class) ||
          source_age_ns >
              requirement.requirement.source_requirement
                  .freshness_budget_ns) {
        return fail_after_dispatch(
            "stop_evidence_source_mismatch",
            "ros2_control rest evidence differs from the retained state "
            "authority contract");
      }
      result.evidence_items.push_back(core::StopEvidenceItemDTO{
          requirement.requirement.evidence_item_id,
          requirement.requirement.evidence_kind,
          core::StopEvidenceItemStateDTO::kSatisfied,
          requirement.resolved_source,
          path.provider_runtime_epoch,
          rest->sequence,
          source_age_ns,
          core::JsonValueDTO{true},
          std::nullopt});
    }
    result.source_epoch = path.provider_runtime_epoch;
    result.source_sequence = rest->sequence;
  }

  result.outcome = core::ParticipantStopStageOutcomeDTO::kSucceeded;
  result.receiver_elapsed_duration_ns =
      std::max<std::int64_t>(0, clock_.now() - started_at);
  return result;
}

core::ControlledStopResultDTO
Ros2ControlExecutionBackend::execute_controlled_stop(
    const core::MotionInterruptStopProfileDTO &profile) {
  return execute_controlled_stop(profile, std::stop_token{});
}

core::ControlledStopResultDTO
Ros2ControlExecutionBackend::execute_controlled_stop(
    const core::MotionInterruptStopProfileDTO &profile,
    std::stop_token stop_token) {
  observe_stop_qualification(
      detail::Ros2ControlStopQualificationEvent::kNominalProviderEntry,
      core::stop_mode_string(profile.stop_mode));
  core::ControlledStopResultDTO result;
  result.executed_stop_mode = profile.stop_mode;
  const std::vector<std::string> joint_names = config_.required_joints;
  const auto reject_profile = [&](std::string reason, std::string message) {
    result.accepted = false;
    result.backend_stop_confirmed = false;
    result.error = core::ErrorDTO{
        std::string(core::kErrorMotionInterruptBackendStopFailed),
        std::move(message),
        {{"reason", std::move(reason)}}};
    return result;
  };
  if (joint_names.empty()) {
    return reject_profile(
        "controlled_stop_joint_scope_empty",
        "ros2_control controlled stop requires a non-empty joint scope");
  }
  if (const auto error =
          core::motion_interrupt_stop_profile_field_error(profile);
      !error.empty()) {
    return reject_profile("motion_interrupt_stop_profile_invalid", error);
  }
  if (!profile.require_backend_confirmation) {
    return reject_profile(
        "motion_interrupt_backend_confirmation_required",
        "ros2_control production stop profiles require backend confirmation");
  }
  if (core::interrupt_structured_value_error(profile.diagnostics).has_value()) {
    return reject_profile(
        "motion_interrupt_stop_profile_diagnostics_invalid",
        "stop-profile diagnostics violate the closed scalar bounds");
  }
  std::string resource_failure_reason;
  if (!angular_stop_acceleration_scope_matches(profile, joint_names,
                                               resource_failure_reason)) {
    return reject_profile(
        std::move(resource_failure_reason),
        "ros2_control stop acceleration limits must exactly cover its angular "
        "joint scope");
  }
  auto settle_velocity_thresholds = angular_settle_velocity_thresholds(
      profile, joint_names, resource_failure_reason);
  if (!settle_velocity_thresholds.has_value()) {
    return reject_profile(
        std::move(resource_failure_reason),
        "ros2_control settle thresholds must name only its angular joint "
        "scope");
  }

  begin_controlled_stop_runtime_scale_freeze(/*assert_unit_scale=*/false);
  const auto runtime_scale_freeze_scope = std::shared_ptr<void>(
      nullptr, [this](void *) { end_controlled_stop_runtime_scale_freeze(); });

  const auto cancelled_result = [&](std::string phase) {
    result.accepted = true;
    result.backend_stop_confirmed = false;
    result.error = core::ErrorDTO{
        std::string(core::kErrorMotionInterruptBackendStopFailed),
        "ros2_control controlled stop was preempted by a later intervention",
        {{"reason", "controlled_stop_cancelled"}, {"phase", std::move(phase)}}};
    return result;
  };
  if (stop_token.stop_requested()) {
    return cancelled_result("entry");
  }

  // Defense in depth: kernel arm-time gate already rejects modes the
  // backend has not declared; this guard is a second line of defense
  // against a mis-wired call. The capability set is the source of
  // truth — re-evaluate it from the current config so deployments
  // that have been reconfigured at runtime cannot slip through.
  const auto current_capability =
      compute_motion_interrupt_stop_capability(config_);
  if (!current_capability.supported_stop_modes.contains(profile.stop_mode)) {
    result.accepted = false;
    result.backend_stop_confirmed = false;
    result.error = core::ErrorDTO{
        std::string(core::kErrorMotionInterruptBackendStopFailed),
        "requested stop_mode is not in this backend's capability profile",
        {{"reason", "stop_mode_not_supported_by_backend"},
         {"backend_capability_profile_id",
          current_capability.capability_profile_id.value_or("")}}};
    return result;
  }

  // Capture the observed joint state BEFORE preempting so the
  // recorded anchor reflects what the controller actually committed
  // to. Use the existing freshness-aware selector so stale samples
  // (older than `joint_state_max_age_s`) fail closed with a
  // structured diagnostic — the kernel relies on this anchor for the
  // signal_trip outcome's `ExecutionStopSnapshotDTO`, and a stale
  // sample would silently certify the wrong stop position.
  std::vector<double> observed_positions;
  std::vector<double> observed_velocities;
  std::string observed_sample_source;
  double observed_sample_age_s = 0.0;
  {
    const auto now = std::chrono::steady_clock::now();
    observe_stop_qualification(
        detail::Ros2ControlStopQualificationEvent::kNominalAdapterLockAttempt,
        "observed_state_anchor");
    std::lock_guard<std::mutex> lock(state_mutex_);
    const auto sample = select_latest_sample_locked(now);
    if (!sample.has_value()) {
      // Build a precise age diagnostic from the freshest available
      // sample (regardless of freshness) so operators can see WHICH
      // source went stale and by how much. When no sample exists at
      // all, report -1 s to distinguish from a fresh-zero age.
      double freshest_age_s = -1.0;
      std::string freshest_source;
      const auto consider = [&](const std::optional<JointSample> &s) {
        if (s.has_value()) {
          const double age =
              std::chrono::duration_cast<std::chrono::duration<double>>(
                  now - s->received_at)
                  .count();
          if (freshest_age_s < 0.0 || age < freshest_age_s) {
            freshest_age_s = age;
            freshest_source = s->source;
          }
        }
      };
      consider(latest_action_feedback_);
      consider(latest_controller_state_);
      consider(latest_joint_state_);
      result.accepted = false;
      result.backend_stop_confirmed = false;
      result.error = core::ErrorDTO{
          std::string(core::kErrorMotionInterruptBackendStopFailed),
          "no fresh observed state available to anchor controlled stop",
          {{"reason", "controlled_stop_no_observed_state"},
           {"max_age_s", std::to_string(config_.joint_state_max_age_s)},
           {"freshest_sample_source", freshest_source},
           {"freshest_sample_age_s", std::to_string(freshest_age_s)}}};
      return result;
    }
    observed_positions = positions_for_joints(*sample, transport_joint_names());
    if (sample->velocities.size() == sample->joint_names.size()) {
      observed_velocities.reserve(joint_names.size());
      for (const auto &joint : transport_joint_names()) {
        const auto it = std::find(sample->joint_names.begin(),
                                  sample->joint_names.end(), joint);
        if (it == sample->joint_names.end()) {
          observed_velocities.clear();
          break;
        }
        const auto idx =
            static_cast<std::size_t>(it - sample->joint_names.begin());
        if (idx >= sample->velocities.size()) {
          observed_velocities.clear();
          break;
        }
        observed_velocities.push_back(sample->velocities[idx]);
      }
    }
    observed_sample_source = sample->source;
    observed_sample_age_s =
        std::chrono::duration_cast<std::chrono::duration<double>>(
            now - sample->received_at)
            .count();
  }
  if (stop_token.stop_requested()) {
    return cancelled_result("observed_state_anchor");
  }
  if (observed_positions.empty() ||
      observed_positions.size() != joint_names.size()) {
    result.accepted = false;
    result.backend_stop_confirmed = false;
    result.error = core::ErrorDTO{
        std::string(core::kErrorMotionInterruptBackendStopFailed),
        "fresh sample is missing required joints for controlled-stop anchor",
        {{"reason", "controlled_stop_anchor_size_mismatch"},
         {"sample_source", observed_sample_source},
         {"sample_age_s", std::to_string(observed_sample_age_s)},
         {"required_joints", std::to_string(joint_names.size())},
         {"sample_positions", std::to_string(observed_positions.size())}}};
    return result;
  }

  const auto execute_soft_on_path_spring_back_substitution =
      [&](std::string reason,
          std::optional<core::ErrorDTO> cause = std::nullopt) {
        // This is the sole qualified internal provider substitution. It is
        // selected before any replacement-goal dispatch and stays inside the
        // nominal provider-contract stage; it is not Kernel-chain fallback.
        std::string cause_code;
        std::string cause_details_str;
        if (cause.has_value()) {
          cause_code = cause->code;
          for (const auto &[detail_key, detail_value] : cause->details) {
            if (!cause_details_str.empty()) {
              cause_details_str += " ";
            }
            cause_details_str += detail_key;
            cause_details_str += "=";
            cause_details_str += detail_value;
          }
        }
        RCLCPP_WARN(
            node_->get_logger(),
            "controlled-stop SUBSTITUTION: soft_on_path -> spring_back_hold "
            "(reason=%s cause_code=%s cause_details=[%s])",
            reason.c_str(), cause_code.c_str(), cause_details_str.c_str());
        core::MotionInterruptStopProfileDTO substituted_profile = profile;
        substituted_profile.stop_mode =
            core::MotionInterruptStopMode::kSpringBackHold;
        substituted_profile.allow_natural_completion_on_short_path = false;
        substituted_profile.recoverable_substitution_mode.reset();
        substituted_profile.deceleration_time_s =
            core::motion_interrupt_deceleration_time_or_default(
                substituted_profile);
        auto substituted =
            execute_controlled_stop(substituted_profile, stop_token);
        substituted.executed_stop_mode =
            core::MotionInterruptStopMode::kSpringBackHold;
        substituted.fallback_reason = reason;
        substituted.diagnostics["requested_stop_mode"] =
            core::interrupt_structured_string("soft_on_path");
        substituted.diagnostics["recoverable_substitution_reason"] =
            core::interrupt_structured_string(reason);
        substituted.diagnostics["recoverable_substitution_pair"] =
            core::interrupt_structured_string("soft_on_path->spring_back_hold");
        if (cause.has_value()) {
          substituted.diagnostics["soft_on_path_error_code"] =
              core::interrupt_structured_string(cause->code);
          substituted.diagnostics["soft_on_path_error_message"] =
              core::interrupt_structured_string(cause->message);
          for (const auto &[key, value] : cause->details) {
            substituted.diagnostics["soft_on_path_error_detail_" + key] =
                core::interrupt_structured_string(value);
          }
          const auto it = cause->details.find("reason");
          if (it != cause->details.end()) {
            substituted.diagnostics["soft_on_path_error_reason"] =
                core::interrupt_structured_string(it->second);
          }
        }
        return substituted;
      };

  const auto soft_on_path_error_recommends_spring_back =
      [](const core::ErrorDTO &error) {
        const auto it = error.details.find("recommended_fallback");
        return it != error.details.end() && it->second == "spring_back_hold";
      };
  const auto fail_soft_on_path_stage = [](std::string reason,
                                          const core::ErrorDTO &cause) {
    core::ControlledStopResultDTO failed;
    failed.accepted = false;
    failed.backend_stop_confirmed = false;
    failed.executed_stop_mode = core::MotionInterruptStopMode::kSoftOnPath;
    failed.stop_command_source =
        core::MotionInterruptStopCommandSource::kDiagnosticOther;
    failed.error = cause;
    failed.diagnostics = structured_text_details(cause.details);
    failed.diagnostics["requested_stop_mode"] =
        core::interrupt_structured_string("soft_on_path");
    failed.diagnostics["reason"] = core::interrupt_structured_string(reason);
    return failed;
  };
  const auto resolve_soft_on_path_planner_failure =
      [&](std::string reason, const core::ErrorDTO &cause) {
        if (profile.recoverable_substitution_mode ==
                core::MotionInterruptStopMode::kSpringBackHold &&
            soft_on_path_error_recommends_spring_back(cause)) {
          return execute_soft_on_path_spring_back_substitution(
              std::move(reason), cause);
        }
        return fail_soft_on_path_stage(std::move(reason), cause);
      };
  const auto terminal_before_replacement_dispatch =
      [&](const char *mode) -> std::optional<core::ControlledStopResultDTO> {
    bool terminal = false;
    bool running = false;
    {
      std::lock_guard<std::mutex> lock(execution_mutex_);
      terminal = terminal_;
      running = running_;
    }
    if (!terminal && running) {
      return std::nullopt;
    }

    const auto terminal_snapshot = get_execution_snapshot();
    core::ControlledStopResultDTO terminal_result;
    terminal_result.accepted =
        terminal_snapshot.state == core::ExecutionState::kCompleted;
    terminal_result.executed_stop_mode = profile.stop_mode;
    terminal_result.backend_stop_confirmed = terminal_result.accepted;
    terminal_result.stop_command_source =
        core::MotionInterruptStopCommandSource::kDiagnosticOther;
    terminal_result.diagnostics["requested_stop_mode"] =
        core::interrupt_structured_string(mode);
    terminal_result.diagnostics["terminal_state"] =
        core::interrupt_structured_string(
            core::to_string(terminal_snapshot.state));
    terminal_result.diagnostics["reason"] = core::interrupt_structured_string(
        terminal_result.accepted
            ? "natural_completion_before_replacement_dispatch"
            : "execution_terminal_before_replacement_dispatch");
    if (!terminal_result.accepted) {
      terminal_result.error = core::ErrorDTO{
          std::string(core::kErrorMotionInterruptBackendStopFailed),
          "ros2_control execution became terminal before replacement stop "
          "goal dispatch",
          {{"reason", "execution_terminal_before_replacement_dispatch"},
           {"terminal_state",
            std::string(core::to_string(terminal_snapshot.state))},
           {"mode", mode}}};
    }
    return terminal_result;
  };

  if (profile.stop_mode == core::MotionInterruptStopMode::kSoftOnPath &&
      config_.enable_soft_on_path_path_ramp) {
    // soft_on_path is NOT gated on the speed-scaling configuration: ManyForge
    // owns the stop timeline. When JTC speed-scaling is active, unit execution
    // scale is published immediately before the preempting replacement-goal
    // dispatch below. That publish/action pair is best-effort ordered by our
    // code, not atomic across DDS/controller internals; target-controller seam
    // qualification must cover the speed-scaling-enabled case. The replacement
    // trajectory already encodes the actual current (scaled) velocity, so the
    // unit-scale assertion is "execute as computed," not a scaling burst.

    std::shared_ptr<const core::TrajectoryDTO> active_trajectory;
    double active_trajectory_time_s = 0.0;
    double active_runtime_scale = 1.0;
    {
      std::lock_guard<std::mutex> lock(execution_mutex_);
      if (!running_ && terminal_) {
        result.accepted = false;
        result.backend_stop_confirmed = false;
        result.error = core::ErrorDTO{
            std::string(core::kErrorMotionInterruptBackendStopFailed),
            "no discrete-trajectory execution in flight",
            {{"reason", "controlled_stop_no_execution_in_flight"}}};
        return result;
      }
      if (!active_trajectory_) {
        return resolve_soft_on_path_planner_failure(
            "soft_on_path_missing_active_trajectory",
            core::ErrorDTO{
                std::string(core::kErrorMotionInterruptBackendStopFailed),
                "soft_on_path requires an active accepted trajectory record",
                {{"reason", "soft_on_path_missing_active_trajectory"}}});
      }
      active_trajectory = active_trajectory_;
      active_trajectory_time_s = active_trajectory_time_s_;
      active_runtime_scale = active_runtime_velocity_scale_;
    }

    // Seam reference time: the instant the command cursor
    // (active_trajectory_time_s) was snapshotted. The compute+dispatch latency
    // measured against this is consumed from the command-frame lead-in just
    // before dispatch (stage A below).
    const auto seam_reference_time = std::chrono::steady_clock::now();

    core::SoftOnPathInitialStateDTO initial;
    initial.state.joint_names = joint_names;
    initial.state.positions_rad = observed_positions;
    initial.position_source = core::SoftOnPathStateSource::kMeasuredFeedback;
    initial.trajectory_time_from_start_s = active_trajectory_time_s;
    initial.runtime_velocity_scale_at_trip = active_runtime_scale;
    initial.sample_age_ns =
        static_cast<std::int64_t>(std::max(0.0, observed_sample_age_s) * 1e9);
    // Anchor velocity source per the per-lane authority
    // (resolve_soft_on_path_anchor_velocity_source): on the commanded-sim lane
    // the reported feedback velocity is decoupled from the commanded motion (it
    // can be opposite-signed), so anchoring the seam on it sign-flips the
    // command. The trajectory model (path tangent x runtime scale) reproduces
    // the commanded velocity, giving a continuous seam. A lane with faithful
    // velocity feedback can force measured feedback via the config.
    const bool has_measured_velocity =
        observed_velocities.size() == joint_names.size();
    initial.velocity_source = resolve_soft_on_path_anchor_velocity_source(
        config_, has_measured_velocity);
    if (initial.velocity_source ==
        core::SoftOnPathStateSource::kMeasuredFeedback) {
      initial.state.velocities_rad_s = observed_velocities;
    }

    core::SoftOnPathPlannerConfig planner_config;
    planner_config.stop_traj_delta_s = config_.soft_on_path_stop_traj_delta_s;
    planner_config.sample_period_s = config_.soft_on_path_sample_period_s;
    planner_config.max_initial_path_deviation_rad =
        config_.soft_on_path_max_initial_path_deviation_rad;
    planner_config.max_initial_velocity_deviation_rad_s =
        config_.soft_on_path_max_initial_velocity_deviation_rad_s;
    planner_config.per_joint_max_jerk_rad_s3 =
        config_.soft_on_path_max_jerk_rad_s3;
    planner_config.allow_model_velocity_fallback =
        config_.allow_soft_on_path_model_velocity_fallback ||
        is_commanded_sim_mode() ||
        initial.velocity_source == core::SoftOnPathStateSource::kModelDerived;

    auto planned = core::plan_soft_on_path_stop(*active_trajectory, profile,
                                                initial, planner_config);
    if (!planned.ok()) {
      if (profile.allow_natural_completion_on_short_path &&
          core::soft_on_path_error_recommends_natural_completion(
              planned.error())) {
        {
          std::lock_guard<std::mutex> lock(execution_mutex_);
          natural_completion_stop_requested_ = true;
        }
        execution_cv_.notify_all();
        const auto natural_completion_scope =
            std::shared_ptr<void>(nullptr, [this](void *) {
              {
                std::lock_guard<std::mutex> lock(execution_mutex_);
                natural_completion_stop_requested_ = false;
              }
              execution_cv_.notify_all();
            });
        (void)natural_completion_scope;
        const auto call_start = std::chrono::steady_clock::now();
        const double wait_s = controlled_stop_wait_timeout_s(
            profile, config_.stop_completion_timeout_s);
        bool terminal = false;
        {
          std::unique_lock<std::mutex> lock(execution_mutex_);
          terminal = execution_cv_.wait_for(
              lock, stop_token,
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::duration<double>(wait_s)),
              [this]() { return terminal_; });
        }
        if (stop_token.stop_requested()) {
          return cancelled_result("soft_on_path_natural_completion_wait");
        }
        const auto call_end = std::chrono::steady_clock::now();
        result.confirmation_latency_ns = std::max<std::int64_t>(
            0, std::chrono::duration_cast<std::chrono::nanoseconds>(call_end -
                                                                    call_start)
                   .count());
        result.stop_command_source =
            core::MotionInterruptStopCommandSource::kDiagnosticOther;
        result.diagnostics = structured_text_details(planned.error().details);
        result.diagnostics["command_source"] =
            core::interrupt_structured_string(
                "ros2_control_natural_completion");
        result.diagnostics["requested_stop_mode"] =
            core::interrupt_structured_string("soft_on_path");
        result.diagnostics["wait_s"] =
            core::interrupt_structured_number(wait_s);
        result.diagnostics["wait_s_source"] = core::interrupt_structured_string(
            "deceleration_time_s_plus_stop_completion_timeout_s");
        const auto terminal_snapshot = get_execution_snapshot();
        if (!terminal ||
            terminal_snapshot.state != core::ExecutionState::kCompleted) {
          result.accepted = true;
          result.backend_stop_confirmed = false;
          result.error = core::ErrorDTO{
              std::string(core::kErrorMotionInterruptBackendStopFailed),
              "ros2_control soft_on_path natural completion did not reach "
              "the trajectory endpoint within the effective stop deadline",
              {{"reason", "soft_on_path_natural_completion_timeout"},
               {"wait_s", std::to_string(wait_s)},
               {"terminal_state",
                std::string(core::to_string(terminal_snapshot.state))}}};
          return result;
        }
        result.accepted = true;
        result.backend_stop_confirmed = true;
        result.completed_by_natural_completion = true;
        return result;
      }
      if (core::soft_on_path_error_recommends_hard_stop(planned.error())) {
        return resolve_soft_on_path_planner_failure(
            "soft_on_path_planner_recommends_hard_stop", planned.error());
      }
      return resolve_soft_on_path_planner_failure(
          planned.error().details.contains("recommended_fallback")
              ? "soft_on_path_planner_rejected"
              : "soft_on_path_planner_failed",
          planned.error());
    }

    // (A) Command-frame dispatch-latency shift (model/command lane). During
    // compute the controller's command advanced along the path, so the
    // lead-in's earliest points fall behind the live command frame: drop the
    // consumed front, shift the rest earlier, and prepend a command-frame
    // continuity pin at the advanced cursor. Done here — before committing the
    // stop flags / deviation tracking — so a guard failure (latency consumed
    // the lead-in budget) escalates to a hard stop from the same clean state as
    // the planner-failure dispositions above. Operates on local copies only
    // (thread-safe). The feedback lane (which anchors at the measured
    // projection, not the command cursor) is dispatched as planned.
    core::TrajectoryDTO dispatch_trajectory =
        planned.value().replacement_trajectory;
    double dispatch_latency_applied_s = 0.0;
    if (planned.value().used_model_velocity) {
      dispatch_latency_applied_s =
          std::chrono::duration_cast<std::chrono::duration<double>>(
              std::chrono::steady_clock::now() - seam_reference_time)
              .count();
      auto shifted = core::shift_soft_stop_for_dispatch_latency(
          planned.value().replacement_trajectory, dispatch_latency_applied_s,
          planner_config.stop_traj_delta_s, *active_trajectory,
          active_trajectory_time_s, std::min(1.0, active_runtime_scale));
      if (!shifted.ok()) {
        return resolve_soft_on_path_planner_failure(
            "soft_on_path_dispatch_latency_consumed_lead_in", shifted.error());
      }
      dispatch_trajectory = std::move(shifted.value());
    }

    if (auto terminal = terminal_before_replacement_dispatch("soft_on_path")) {
      return *terminal;
    }
    if (stop_token.stop_requested()) {
      return cancelled_result("soft_on_path_before_dispatch");
    }

    begin_controlled_stop_path_deviation_tracking(
        active_trajectory, planned.value().path_deviation_max_rad);
    const auto call_start = std::chrono::steady_clock::now();
    {
      std::lock_guard<std::mutex> lock(execution_mutex_);
      stop_requested_ = true;
      natural_completion_stop_requested_ = false;
      pause_requested_ = false;
      stop_requested_at_ = std::chrono::steady_clock::now();
      controlled_stop_preempt_in_progress_ = true;
      controlled_stop_command_reference_.reset();
      // The replacement FJT goal preempts the original at the controller
      // boundary; old-goal terminal callbacks route through stop_requested_.
    }

    // Assert unit execution scale immediately before the preempting replacement
    // goal so JTC speed-scaling does not retime the computed wall-time stop.
    // The trajectory already encodes the current scaled seam velocity; the
    // assertion means "execute this authored stop as computed", not a scaling
    // burst. The controlled-stop scale guard restores/dequeues operator intent
    // when the stop window ends.
    assert_controlled_stop_unit_runtime_scale();

    const auto dispatch_start = std::chrono::steady_clock::now();
    const bool dispatched = send_fjt_replacement_trajectory(
        dispatch_trajectory, /*track_terminal_goal=*/true);
    if (stop_token.stop_requested()) {
      clear_managed_controlled_stop_state();
      clear_controlled_stop_path_deviation_tracking();
      return cancelled_result("soft_on_path_dispatch");
    }
    const auto dispatch_accepted = std::chrono::steady_clock::now();
    const auto backend_call_to_goal_dispatch_ns = std::max<std::int64_t>(
        0, std::chrono::duration_cast<std::chrono::nanoseconds>(dispatch_start -
                                                                call_start)
               .count());
    const auto goal_dispatch_to_accept_ns = std::max<std::int64_t>(
        0, std::chrono::duration_cast<std::chrono::nanoseconds>(
               dispatch_accepted - dispatch_start)
               .count());
    const auto backend_call_to_goal_accept_ns = std::max<std::int64_t>(
        0, std::chrono::duration_cast<std::chrono::nanoseconds>(
               dispatch_accepted - call_start)
               .count());
    const auto stop_delta_budget_ns = static_cast<std::int64_t>(
        std::max(0.0, config_.soft_on_path_stop_traj_delta_s) * 1e9);
    if (!dispatched) {
      std::optional<core::ErrorDTO> dispatch_error;
      {
        std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
        dispatch_error = managed_fjt_terminal_error_;
      }
      auto error = dispatch_error.value_or(core::ErrorDTO{
          std::string(core::kErrorMotionInterruptBackendStopFailed),
          "failed to dispatch soft_on_path path-ramp replacement trajectory",
          {{"reason", "soft_on_path_path_ramp_dispatch_failed"}}});
      error.details["configured_stop_traj_delta_ns"] =
          std::to_string(stop_delta_budget_ns);
      error.details["backend_call_to_goal_accept_ns"] =
          std::to_string(backend_call_to_goal_accept_ns);
      error.details["backend_call_to_goal_dispatch_ns"] =
          std::to_string(backend_call_to_goal_dispatch_ns);
      error.details["goal_dispatch_to_accept_ns"] =
          std::to_string(goal_dispatch_to_accept_ns);
      clear_managed_controlled_stop_state();
      clear_controlled_stop_path_deviation_tracking();
      return fail_soft_on_path_stage("soft_on_path_path_ramp_dispatch_failed",
                                     error);
    }
    if (stop_delta_budget_ns > 0 &&
        backend_call_to_goal_accept_ns > stop_delta_budget_ns) {
      GoalHandleFJT::SharedPtr path_ramp_goal;
      {
        std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
        path_ramp_goal = managed_fjt_goal_;
      }
      cancel_goal_if_known(path_ramp_goal);
      clear_managed_controlled_stop_state();
      clear_controlled_stop_path_deviation_tracking();
      return fail_soft_on_path_stage(
          "soft_on_path_stop_delta_budget_exceeded",
          core::ErrorDTO{
              std::string(core::kErrorMotionInterruptBackendStopFailed),
              "soft_on_path path-ramp goal was accepted after the configured "
              "handoff budget",
              {{"reason", "soft_on_path_stop_delta_budget_exceeded"},
               {"configured_stop_traj_delta_ns",
                std::to_string(stop_delta_budget_ns)},
               {"backend_call_to_goal_accept_ns",
                std::to_string(backend_call_to_goal_accept_ns)},
               {"backend_call_to_goal_dispatch_ns",
                std::to_string(backend_call_to_goal_dispatch_ns)},
               {"goal_dispatch_to_accept_ns",
                std::to_string(goal_dispatch_to_accept_ns)}}});
    }
    install_controlled_stop_command_reference(dispatch_trajectory,
                                              dispatch_accepted);

    const double wait_s = controlled_stop_wait_timeout_s(
        profile, config_.stop_completion_timeout_s);
    bool received = false;
    bool success = false;
    std::optional<core::ErrorDTO> terminal_error;
    std::optional<std::chrono::steady_clock::time_point>
        terminal_result_received_at;
    observe_stop_qualification(
        detail::Ros2ControlStopQualificationEvent::kNominalResultWaitEnter,
        "soft_on_path_terminal_result");
    {
      std::unique_lock<std::mutex> lock(managed_fjt_mutex_);
      received = managed_fjt_cv_.wait_for(
          lock, stop_token,
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::duration<double>(wait_s)),
          [this]() { return managed_fjt_terminal_result_received_; });
      success = managed_fjt_terminal_result_success_;
      terminal_error = managed_fjt_terminal_error_;
      terminal_result_received_at = managed_fjt_terminal_result_received_at_;
    }
    if (stop_token.stop_requested()) {
      GoalHandleFJT::SharedPtr path_ramp_goal;
      {
        std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
        path_ramp_goal = managed_fjt_goal_;
      }
      cancel_goal_if_known(path_ramp_goal);
      clear_managed_controlled_stop_state();
      clear_controlled_stop_path_deviation_tracking();
      return cancelled_result("soft_on_path_terminal_wait");
    }
    const auto call_end = std::chrono::steady_clock::now();
    result.confirmation_latency_ns = std::max<std::int64_t>(
        0, std::chrono::duration_cast<std::chrono::nanoseconds>(call_end -
                                                                call_start)
               .count());
    if (!received) {
      GoalHandleFJT::SharedPtr path_ramp_goal;
      {
        std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
        path_ramp_goal = managed_fjt_goal_;
      }
      cancel_goal_if_known(path_ramp_goal);
      result.accepted = true;
      result.backend_stop_confirmed = false;
      result.error = core::ErrorDTO{
          std::string(core::kErrorMotionInterruptBackendStopFailed),
          "controller did not confirm soft_on_path path-ramp goal within the "
          "deadline",
          {{"reason", "backend_stop_not_confirmed"},
           {"wait_s", std::to_string(wait_s)},
           {"mode", "soft_on_path"}}};
      clear_managed_controlled_stop_state();
      clear_controlled_stop_path_deviation_tracking();
      return result;
    }
    if (!success) {
      result.accepted = true;
      result.backend_stop_confirmed = false;
      result.error = terminal_error.value_or(core::ErrorDTO{
          std::string(core::kErrorMotionInterruptBackendStopFailed),
          "soft_on_path path-ramp goal terminated unsuccessfully",
          {{"reason", "controller_terminal_result_unsuccessful"},
           {"mode", "soft_on_path"}}});
      clear_managed_controlled_stop_state();
      clear_controlled_stop_path_deviation_tracking();
      return result;
    }

    const auto post_stop_deadline =
        call_start +
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(wait_s));
    std::optional<JointSample> post_stop_sample;
    if (terminal_result_received_at.has_value()) {
      post_stop_sample = wait_for_fresh_passive_sample_after(
          *terminal_result_received_at, post_stop_deadline, stop_token);
    }
    std::vector<double> post_stop_positions;
    std::vector<double> post_stop_velocities;
    std::string post_stop_source;
    std::int64_t post_stop_source_timestamp_ns = 0;
    if (post_stop_sample.has_value()) {
      post_stop_positions =
          positions_for_joints(*post_stop_sample, transport_joint_names());
      post_stop_velocities =
          velocities_for_joints(*post_stop_sample, transport_joint_names());
      post_stop_source = post_stop_sample->source;
      post_stop_source_timestamp_ns = post_stop_sample->received_timestamp_ns;
    }
    if (post_stop_positions.empty() ||
        post_stop_positions.size() != joint_names.size()) {
      result.accepted = true;
      result.backend_stop_confirmed = false;
      result.error = core::ErrorDTO{
          std::string(core::kErrorMotionInterruptStopStateInvalid),
          "controller confirmed soft_on_path path ramp but no fresh post-stop "
          "sample is available",
          {{"reason", "controlled_stop_post_hold_state_unavailable"},
           {"sample_source", post_stop_source},
           {"mode", "soft_on_path"}}};
      clear_managed_controlled_stop_state();
      clear_controlled_stop_path_deviation_tracking();
      return result;
    }
    double path_deviation_max_rad = planned.value().path_deviation_max_rad;
    std::size_t path_deviation_feedback_samples = 0;
    std::optional<core::ErrorDTO> path_deviation_feedback_error;
    {
      std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
      if (controlled_stop_path_deviation_max_rad_.has_value()) {
        path_deviation_max_rad = std::max(
            path_deviation_max_rad, *controlled_stop_path_deviation_max_rad_);
      }
      path_deviation_feedback_samples =
          controlled_stop_path_deviation_feedback_samples_;
      path_deviation_feedback_error = controlled_stop_path_deviation_error_;
    }
    core::RobotStateDTO post_stop_state;
    post_stop_state.joint_names = joint_names;
    post_stop_state.positions_rad = post_stop_positions;
    auto post_stop_deviation = core::measure_soft_on_path_path_deviation(
        *active_trajectory, post_stop_state);
    if (post_stop_deviation.ok()) {
      path_deviation_max_rad =
          std::max(path_deviation_max_rad, post_stop_deviation.value());
    } else {
      path_deviation_feedback_error = post_stop_deviation.error();
    }
    double stop_distance_sq = 0.0;
    for (std::size_t i = 0; i < joint_names.size(); ++i) {
      const double delta = post_stop_positions[i] - observed_positions[i];
      stop_distance_sq += delta * delta;
    }
    std::vector<double> snapshot_positions = post_stop_positions;
    std::vector<double> snapshot_velocities = post_stop_velocities;
    std::string snapshot_source;
    if (is_commanded_sim_mode() && !dispatch_trajectory.waypoints.empty() &&
        dispatch_trajectory.waypoints.back().positions_rad.size() ==
            joint_names.size()) {
      // In commanded-sim the Composer visualizer and the next planning start
      // state are command-frame authoritative. The measured/controller sample
      // is still captured above for settle evidence, but publishing it here
      // would make the visual robot jump from the stop command reference to a
      // slightly different simulator/controller sample after the stop confirms.
      snapshot_positions = dispatch_trajectory.waypoints.back().positions_rad;
      snapshot_velocities =
          dispatch_trajectory.waypoints.back().velocities_rad_s;
      snapshot_source = "commanded_sim_controlled_stop_terminal";
    }
    const auto snapshot_before_publish = get_execution_snapshot();
    const auto terminal_generation =
        complete_managed_controlled_stop_execution();
    const auto terminal_scene_source_ns = uses_authored_backend_state()
                                              ? clock_.now()
                                              : post_stop_source_timestamp_ns;
    if (uses_authored_backend_state()) {
      (void)publish_snapshot(
          core::ExecutionState::kStopped, snapshot_before_publish.progress,
          snapshot_positions, std::nullopt, snapshot_source,
          terminal_generation, SnapshotPublicationAuthority::kBackendAuthored,
          snapshot_velocities.empty() ? nullptr : &snapshot_velocities);
    } else {
      (void)publish_observed_snapshot(
          core::ExecutionState::kStopped, snapshot_before_publish.progress,
          snapshot_positions, snapshot_velocities,
          post_stop_source_timestamp_ns, std::nullopt, terminal_generation);
    }
    project_terminal_scene_positions(snapshot_positions,
                                     terminal_scene_source_ns);

    result.accepted = true;
    result.backend_stop_confirmed = true;
    result.executed_stop_mode = core::MotionInterruptStopMode::kSoftOnPath;
    result.stop_command_source =
        core::MotionInterruptStopCommandSource::kFjtPreemptPathRamp;
    // A successful on-path soft stop is observable too (the symmetric
    // counterpart to the spring-back escalation WARN): log which seam ran and
    // the command-vs-measured cursor it anchored on, so operators and logs can
    // verify the command-frame path executed without polling the event stream.
    {
      const auto &plan_diag = planned.value().diagnostics;
      const auto diag = [&plan_diag](const char *key) {
        const auto it = plan_diag.find(key);
        return it != plan_diag.end()
                   ? core::interrupt_structured_scalar_text(it->second)
                   : std::string("?");
      };
      RCLCPP_INFO(
          node_->get_logger(),
          "controlled-stop CONFIRMED soft_on_path via fjt_preempt_path_ramp "
          "(seam=%s seam_cursor_s=%s measured_projection_s=%s "
          "effective_ramp_s=%s "
          "dispatch_latency_shift_s=%.4f)",
          diag("seam").c_str(), diag("seam_cursor_s").c_str(),
          diag("measured_projection_s").c_str(),
          diag("effective_ramp_s").c_str(), dispatch_latency_applied_s);
    }
    result.stop_distance_rad = std::sqrt(stop_distance_sq);
    result.initial_path_deviation_rad =
        planned.value().initial_path_deviation_rad;
    result.path_deviation_max_rad = path_deviation_max_rad;
    result.diagnostics = planned.value().diagnostics;
    result.diagnostics["runtime_scale_isolation"] =
        core::interrupt_structured_string(
            config_.enable_jtc_speed_scaling_input
                ? "owned_scale_asserted_unit_during_stop"
                : "wall_rate_no_controller_scaling");
    result.diagnostics["timeline_authority_tier"] =
        core::interrupt_structured_string(config_.enable_jtc_speed_scaling_input
                                              ? "owned_scale_asserted"
                                              : "wall_rate");
    result.diagnostics["runtime_velocity_scale_at_trip"] =
        core::interrupt_structured_number(active_runtime_scale);
    result.diagnostics["stop_command_source"] =
        core::interrupt_structured_string("fjt_preempt_path_ramp");
    result.diagnostics["configured_stop_traj_delta_ns"] =
        core::interrupt_structured_number(
            static_cast<double>(stop_delta_budget_ns));
    result.diagnostics["backend_call_to_goal_dispatch_ns"] =
        core::interrupt_structured_number(
            static_cast<double>(backend_call_to_goal_dispatch_ns));
    result.diagnostics["goal_dispatch_to_accept_ns"] =
        core::interrupt_structured_number(
            static_cast<double>(goal_dispatch_to_accept_ns));
    result.diagnostics["backend_call_to_goal_accept_ns"] =
        core::interrupt_structured_number(
            static_cast<double>(backend_call_to_goal_accept_ns));
    result.diagnostics["stop_traj_delta_budget_margin_ns"] =
        core::interrupt_structured_number(static_cast<double>(
            stop_delta_budget_ns - backend_call_to_goal_accept_ns));
    result.diagnostics["post_stop_sample_source"] =
        core::interrupt_structured_string(post_stop_source);
    result.diagnostics
        ["path_deviation_source"] = core::interrupt_structured_string(
        path_deviation_feedback_samples > 0
            ? "action_feedback_plus_post_stop_sample"
            : "post_stop_sample_plus_planner_projection_no_action_feedback");
    result.diagnostics["path_deviation_feedback_samples"] =
        core::interrupt_structured_number(
            static_cast<double>(path_deviation_feedback_samples));
    result.diagnostics["path_deviation_max_rad"] =
        core::interrupt_structured_number(path_deviation_max_rad);
    if (path_deviation_feedback_error.has_value()) {
      result.diagnostics["path_deviation_feedback_error_code"] =
          core::interrupt_structured_string(
              path_deviation_feedback_error->code);
      result.diagnostics["path_deviation_feedback_error_message"] =
          core::interrupt_structured_string(
              path_deviation_feedback_error->message);
      const auto it = path_deviation_feedback_error->details.find("reason");
      if (it != path_deviation_feedback_error->details.end()) {
        result.diagnostics["path_deviation_feedback_error_reason"] =
            core::interrupt_structured_string(it->second);
      }
    }
    clear_controlled_stop_path_deviation_tracking();
    return result;
  }

  if (profile.stop_mode == core::MotionInterruptStopMode::kSpringBackHold) {
    std::shared_ptr<const core::TrajectoryDTO> active_trajectory;
    double active_trajectory_time_s = 0.0;
    double active_runtime_scale = 1.0;
    {
      std::lock_guard<std::mutex> lock(execution_mutex_);
      if (!running_ && terminal_) {
        result.accepted = false;
        result.backend_stop_confirmed = false;
        result.error = core::ErrorDTO{
            std::string(core::kErrorMotionInterruptBackendStopFailed),
            "no discrete-trajectory execution in flight",
            {{"reason", "controlled_stop_no_execution_in_flight"}}};
        return result;
      }
      if (!active_trajectory_) {
        result.accepted = false;
        result.backend_stop_confirmed = false;
        result.error = core::ErrorDTO{
            std::string(core::kErrorMotionInterruptBackendStopFailed),
            "spring_back_hold requires an active accepted trajectory record",
            {{"reason", "spring_back_missing_active_trajectory"}}};
        return result;
      }
      active_trajectory = active_trajectory_;
      active_trajectory_time_s = active_trajectory_time_s_;
      active_runtime_scale = active_runtime_velocity_scale_;
    }

    const auto seam_reference_time = std::chrono::steady_clock::now();

    core::SoftOnPathInitialStateDTO initial;
    initial.state.joint_names = joint_names;
    initial.state.positions_rad = observed_positions;
    initial.position_source = core::SoftOnPathStateSource::kMeasuredFeedback;
    initial.trajectory_time_from_start_s = active_trajectory_time_s;
    initial.runtime_velocity_scale_at_trip = active_runtime_scale;
    initial.sample_age_ns =
        static_cast<std::int64_t>(std::max(0.0, observed_sample_age_s) * 1e9);
    const bool has_measured_velocity =
        observed_velocities.size() == joint_names.size();
    initial.velocity_source = resolve_soft_on_path_anchor_velocity_source(
        config_, has_measured_velocity);
    if (initial.velocity_source ==
        core::SoftOnPathStateSource::kMeasuredFeedback) {
      initial.state.velocities_rad_s = observed_velocities;
    }

    core::SoftOnPathPlannerConfig planner_config;
    planner_config.stop_traj_delta_s = config_.soft_on_path_stop_traj_delta_s;
    planner_config.sample_period_s = config_.soft_on_path_sample_period_s;
    planner_config.max_initial_path_deviation_rad =
        config_.soft_on_path_max_initial_path_deviation_rad;
    planner_config.max_initial_velocity_deviation_rad_s =
        config_.soft_on_path_max_initial_velocity_deviation_rad_s;
    planner_config.per_joint_max_jerk_rad_s3 =
        config_.soft_on_path_max_jerk_rad_s3;
    planner_config.allow_model_velocity_fallback =
        config_.allow_soft_on_path_model_velocity_fallback ||
        is_commanded_sim_mode() ||
        initial.velocity_source == core::SoftOnPathStateSource::kModelDerived;

    core::MotionInterruptStopProfileDTO spring_profile = profile;
    spring_profile.deceleration_time_s =
        core::motion_interrupt_deceleration_time_or_default(profile);
    auto planned = core::plan_spring_back_on_path_stop(
        *active_trajectory, spring_profile, initial, planner_config);
    if (!planned.ok()) {
      result.accepted = false;
      result.backend_stop_confirmed = false;
      result.error = planned.error();
      return result;
    }

    core::TrajectoryDTO dispatch_trajectory =
        planned.value().replacement_trajectory;
    double dispatch_latency_applied_s = 0.0;
    if (planned.value().used_model_velocity) {
      dispatch_latency_applied_s =
          std::chrono::duration_cast<std::chrono::duration<double>>(
              std::chrono::steady_clock::now() - seam_reference_time)
              .count();
      auto shifted = core::shift_soft_stop_for_dispatch_latency(
          planned.value().replacement_trajectory, dispatch_latency_applied_s,
          planner_config.stop_traj_delta_s, *active_trajectory,
          active_trajectory_time_s, std::min(1.0, active_runtime_scale));
      if (!shifted.ok()) {
        result.accepted = false;
        result.backend_stop_confirmed = false;
        result.error = shifted.error();
        return result;
      }
      dispatch_trajectory = std::move(shifted.value());
    }

    if (auto terminal =
            terminal_before_replacement_dispatch("spring_back_hold")) {
      return *terminal;
    }
    if (stop_token.stop_requested()) {
      return cancelled_result("spring_back_before_dispatch");
    }

    begin_controlled_stop_path_deviation_tracking(
        active_trajectory, planned.value().path_deviation_max_rad);
    const auto call_start = std::chrono::steady_clock::now();
    {
      std::lock_guard<std::mutex> lock(execution_mutex_);
      stop_requested_ = true;
      natural_completion_stop_requested_ = false;
      pause_requested_ = false;
      stop_requested_at_ = std::chrono::steady_clock::now();
      controlled_stop_preempt_in_progress_ = true;
      controlled_stop_command_reference_.reset();
    }

    assert_controlled_stop_unit_runtime_scale();

    const auto dispatch_start = std::chrono::steady_clock::now();
    const bool dispatched = send_fjt_replacement_trajectory(
        dispatch_trajectory, /*track_terminal_goal=*/true);
    if (stop_token.stop_requested()) {
      clear_managed_controlled_stop_state();
      clear_controlled_stop_path_deviation_tracking();
      return cancelled_result("spring_back_dispatch");
    }
    const auto dispatch_accepted = std::chrono::steady_clock::now();
    const auto backend_call_to_goal_dispatch_ns = std::max<std::int64_t>(
        0, std::chrono::duration_cast<std::chrono::nanoseconds>(dispatch_start -
                                                                call_start)
               .count());
    const auto goal_dispatch_to_accept_ns = std::max<std::int64_t>(
        0, std::chrono::duration_cast<std::chrono::nanoseconds>(
               dispatch_accepted - dispatch_start)
               .count());
    const auto backend_call_to_goal_accept_ns = std::max<std::int64_t>(
        0, std::chrono::duration_cast<std::chrono::nanoseconds>(
               dispatch_accepted - call_start)
               .count());
    const auto stop_delta_budget_ns = static_cast<std::int64_t>(
        std::max(0.0, config_.soft_on_path_stop_traj_delta_s) * 1e9);
    if (!dispatched) {
      std::optional<core::ErrorDTO> dispatch_error;
      {
        std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
        dispatch_error = managed_fjt_terminal_error_;
      }
      result.accepted = false;
      result.backend_stop_confirmed = false;
      result.error = dispatch_error.value_or(core::ErrorDTO{
          std::string(core::kErrorMotionInterruptBackendStopFailed),
          "failed to dispatch spring_back_hold on-path replacement trajectory",
          {{"reason", "spring_back_path_ramp_dispatch_failed"}}});
      clear_managed_controlled_stop_state();
      clear_controlled_stop_path_deviation_tracking();
      return result;
    }
    if (stop_delta_budget_ns > 0 &&
        backend_call_to_goal_accept_ns > stop_delta_budget_ns) {
      GoalHandleFJT::SharedPtr path_ramp_goal;
      {
        std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
        path_ramp_goal = managed_fjt_goal_;
      }
      cancel_goal_if_known(path_ramp_goal);
      result.accepted = false;
      result.backend_stop_confirmed = false;
      result.error = core::ErrorDTO{
          std::string(core::kErrorMotionInterruptBackendStopFailed),
          "spring_back_hold path-ramp goal was accepted after the configured "
          "handoff budget",
          {{"reason", "spring_back_stop_delta_budget_exceeded"},
           {"recommended_disposition", "hard_stop"},
           {"configured_stop_traj_delta_ns",
            std::to_string(stop_delta_budget_ns)},
           {"backend_call_to_goal_accept_ns",
            std::to_string(backend_call_to_goal_accept_ns)},
           {"backend_call_to_goal_dispatch_ns",
            std::to_string(backend_call_to_goal_dispatch_ns)},
           {"goal_dispatch_to_accept_ns",
            std::to_string(goal_dispatch_to_accept_ns)}}};
      clear_managed_controlled_stop_state();
      clear_controlled_stop_path_deviation_tracking();
      return result;
    }
    install_controlled_stop_command_reference(dispatch_trajectory,
                                              dispatch_accepted);

    const double wait_s = controlled_stop_wait_timeout_s(
        profile, config_.stop_completion_timeout_s);
    bool received = false;
    bool success = false;
    std::optional<core::ErrorDTO> terminal_error;
    std::optional<std::chrono::steady_clock::time_point>
        terminal_result_received_at;
    observe_stop_qualification(
        detail::Ros2ControlStopQualificationEvent::kNominalResultWaitEnter,
        "spring_back_hold_terminal_result");
    {
      std::unique_lock<std::mutex> lock(managed_fjt_mutex_);
      received = managed_fjt_cv_.wait_for(
          lock, stop_token,
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::duration<double>(wait_s)),
          [this]() { return managed_fjt_terminal_result_received_; });
      success = managed_fjt_terminal_result_success_;
      terminal_error = managed_fjt_terminal_error_;
      terminal_result_received_at = managed_fjt_terminal_result_received_at_;
    }
    if (stop_token.stop_requested()) {
      GoalHandleFJT::SharedPtr path_ramp_goal;
      {
        std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
        path_ramp_goal = managed_fjt_goal_;
      }
      cancel_goal_if_known(path_ramp_goal);
      clear_managed_controlled_stop_state();
      clear_controlled_stop_path_deviation_tracking();
      return cancelled_result("spring_back_terminal_wait");
    }
    const auto call_end = std::chrono::steady_clock::now();
    result.confirmation_latency_ns = std::max<std::int64_t>(
        0, std::chrono::duration_cast<std::chrono::nanoseconds>(call_end -
                                                                call_start)
               .count());
    if (!received) {
      GoalHandleFJT::SharedPtr path_ramp_goal;
      {
        std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
        path_ramp_goal = managed_fjt_goal_;
      }
      cancel_goal_if_known(path_ramp_goal);
      result.accepted = true;
      result.backend_stop_confirmed = false;
      result.error = core::ErrorDTO{
          std::string(core::kErrorMotionInterruptBackendStopFailed),
          "controller did not confirm spring_back_hold path-ramp goal within "
          "the deadline",
          {{"reason", "backend_stop_not_confirmed"},
           {"wait_s", std::to_string(wait_s)},
           {"mode", "spring_back_hold"}}};
      clear_managed_controlled_stop_state();
      clear_controlled_stop_path_deviation_tracking();
      return result;
    }
    if (!success) {
      result.accepted = true;
      result.backend_stop_confirmed = false;
      result.error = terminal_error.value_or(core::ErrorDTO{
          std::string(core::kErrorMotionInterruptBackendStopFailed),
          "spring_back_hold path-ramp goal terminated unsuccessfully",
          {{"reason", "controller_terminal_result_unsuccessful"},
           {"mode", "spring_back_hold"}}});
      clear_managed_controlled_stop_state();
      clear_controlled_stop_path_deviation_tracking();
      return result;
    }

    const auto post_stop_deadline =
        call_start +
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(wait_s));
    std::optional<JointSample> post_stop_sample;
    if (terminal_result_received_at.has_value()) {
      post_stop_sample = wait_for_fresh_passive_sample_after(
          *terminal_result_received_at, post_stop_deadline, stop_token);
    }
    std::vector<double> post_stop_positions;
    std::vector<double> post_stop_velocities;
    std::string post_stop_source;
    std::int64_t post_stop_source_timestamp_ns = 0;
    if (post_stop_sample.has_value()) {
      post_stop_positions =
          positions_for_joints(*post_stop_sample, transport_joint_names());
      post_stop_velocities =
          velocities_for_joints(*post_stop_sample, transport_joint_names());
      post_stop_source = post_stop_sample->source;
      post_stop_source_timestamp_ns = post_stop_sample->received_timestamp_ns;
    }
    if (post_stop_positions.empty() ||
        post_stop_positions.size() != joint_names.size()) {
      result.accepted = true;
      result.backend_stop_confirmed = false;
      result.error = core::ErrorDTO{
          std::string(core::kErrorMotionInterruptStopStateInvalid),
          "controller confirmed spring_back_hold path ramp but no fresh "
          "post-stop sample is available",
          {{"reason", "controlled_stop_post_hold_state_unavailable"},
           {"sample_source", post_stop_source},
           {"mode", "spring_back_hold"}}};
      clear_managed_controlled_stop_state();
      clear_controlled_stop_path_deviation_tracking();
      return result;
    }
    double path_deviation_max_rad = planned.value().path_deviation_max_rad;
    std::size_t path_deviation_feedback_samples = 0;
    std::optional<core::ErrorDTO> path_deviation_feedback_error;
    {
      std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
      if (controlled_stop_path_deviation_max_rad_.has_value()) {
        path_deviation_max_rad = std::max(
            path_deviation_max_rad, *controlled_stop_path_deviation_max_rad_);
      }
      path_deviation_feedback_samples =
          controlled_stop_path_deviation_feedback_samples_;
      path_deviation_feedback_error = controlled_stop_path_deviation_error_;
    }
    core::RobotStateDTO post_stop_state;
    post_stop_state.joint_names = joint_names;
    post_stop_state.positions_rad = post_stop_positions;
    auto post_stop_deviation = core::measure_soft_on_path_path_deviation(
        *active_trajectory, post_stop_state);
    if (post_stop_deviation.ok()) {
      path_deviation_max_rad =
          std::max(path_deviation_max_rad, post_stop_deviation.value());
    } else {
      path_deviation_feedback_error = post_stop_deviation.error();
    }
    double stop_distance_sq = 0.0;
    for (std::size_t i = 0; i < joint_names.size(); ++i) {
      const double delta = post_stop_positions[i] - observed_positions[i];
      stop_distance_sq += delta * delta;
    }
    std::vector<double> snapshot_positions = post_stop_positions;
    std::vector<double> snapshot_velocities = post_stop_velocities;
    std::string snapshot_source;
    if (is_commanded_sim_mode() && !dispatch_trajectory.waypoints.empty() &&
        dispatch_trajectory.waypoints.back().positions_rad.size() ==
            joint_names.size()) {
      snapshot_positions = dispatch_trajectory.waypoints.back().positions_rad;
      snapshot_velocities =
          dispatch_trajectory.waypoints.back().velocities_rad_s;
      snapshot_source = "commanded_sim_spring_back_path_terminal";
    }
    const auto snapshot_before_publish = get_execution_snapshot();
    const auto terminal_generation =
        complete_managed_controlled_stop_execution();
    const auto terminal_scene_source_ns = uses_authored_backend_state()
                                              ? clock_.now()
                                              : post_stop_source_timestamp_ns;
    if (uses_authored_backend_state()) {
      (void)publish_snapshot(
          core::ExecutionState::kStopped, snapshot_before_publish.progress,
          snapshot_positions, std::nullopt, snapshot_source,
          terminal_generation, SnapshotPublicationAuthority::kBackendAuthored,
          snapshot_velocities.empty() ? nullptr : &snapshot_velocities);
    } else {
      (void)publish_observed_snapshot(
          core::ExecutionState::kStopped, snapshot_before_publish.progress,
          snapshot_positions, snapshot_velocities,
          post_stop_source_timestamp_ns, std::nullopt, terminal_generation);
    }
    project_terminal_scene_positions(snapshot_positions,
                                     terminal_scene_source_ns);

    result.accepted = true;
    result.backend_stop_confirmed = true;
    result.executed_stop_mode = core::MotionInterruptStopMode::kSpringBackHold;
    result.stop_command_source =
        core::MotionInterruptStopCommandSource::kFjtPreemptPathRamp;
    result.stop_distance_rad = std::sqrt(stop_distance_sq);
    result.initial_path_deviation_rad =
        planned.value().initial_path_deviation_rad;
    result.path_deviation_max_rad = path_deviation_max_rad;
    result.diagnostics = planned.value().diagnostics;
    result.diagnostics["runtime_scale_isolation"] =
        core::interrupt_structured_string(
            config_.enable_jtc_speed_scaling_input
                ? "owned_scale_asserted_unit_during_stop"
                : "wall_rate_no_controller_scaling");
    result.diagnostics["timeline_authority_tier"] =
        core::interrupt_structured_string(config_.enable_jtc_speed_scaling_input
                                              ? "owned_scale_asserted"
                                              : "wall_rate");
    result.diagnostics["runtime_velocity_scale_at_trip"] =
        core::interrupt_structured_number(active_runtime_scale);
    result.diagnostics["stop_command_source"] =
        core::interrupt_structured_string("fjt_preempt_spring_back_path_ramp");
    result.diagnostics["configured_stop_traj_delta_ns"] =
        core::interrupt_structured_number(
            static_cast<double>(stop_delta_budget_ns));
    result.diagnostics["backend_call_to_goal_dispatch_ns"] =
        core::interrupt_structured_number(
            static_cast<double>(backend_call_to_goal_dispatch_ns));
    result.diagnostics["goal_dispatch_to_accept_ns"] =
        core::interrupt_structured_number(
            static_cast<double>(goal_dispatch_to_accept_ns));
    result.diagnostics["backend_call_to_goal_accept_ns"] =
        core::interrupt_structured_number(
            static_cast<double>(backend_call_to_goal_accept_ns));
    result.diagnostics["stop_traj_delta_budget_margin_ns"] =
        core::interrupt_structured_number(static_cast<double>(
            stop_delta_budget_ns - backend_call_to_goal_accept_ns));
    result.diagnostics["dispatch_latency_shift_s"] =
        core::interrupt_structured_number(dispatch_latency_applied_s);
    result.diagnostics["post_stop_sample_source"] =
        core::interrupt_structured_string(post_stop_source);
    result.diagnostics
        ["path_deviation_source"] = core::interrupt_structured_string(
        path_deviation_feedback_samples > 0
            ? "action_feedback_plus_post_stop_sample"
            : "post_stop_sample_plus_planner_projection_no_action_feedback");
    result.diagnostics["path_deviation_feedback_samples"] =
        core::interrupt_structured_number(
            static_cast<double>(path_deviation_feedback_samples));
    result.diagnostics["path_deviation_max_rad"] =
        core::interrupt_structured_number(path_deviation_max_rad);
    if (path_deviation_feedback_error.has_value()) {
      result.diagnostics["path_deviation_feedback_error_code"] =
          core::interrupt_structured_string(
              path_deviation_feedback_error->code);
      result.diagnostics["path_deviation_feedback_error_message"] =
          core::interrupt_structured_string(
              path_deviation_feedback_error->message);
      const auto it = path_deviation_feedback_error->details.find("reason");
      if (it != path_deviation_feedback_error->details.end()) {
        result.diagnostics["path_deviation_feedback_error_reason"] =
            core::interrupt_structured_string(it->second);
      }
    }
    clear_controlled_stop_path_deviation_tracking();
    return result;
  }

  // The kernel's arm-time + trip-time invariant work has already
  // selected this trip path; mark the discrete-trajectory state
  // machine so the OLD action goal's terminal callback routes through
  // the cancel branch (not fail_execution).
  const auto cancel_start_ns = std::chrono::steady_clock::now();
  GoalHandleFJT::SharedPtr active_goal_to_cancel;
  std::uint64_t jtc_cancel_reference_token = 0;
  bool jtc_cancel_reference_authority_busy = false;
  {
    std::lock_guard<std::mutex> lock(execution_mutex_);
    if (!running_ && terminal_) {
      result.accepted = false;
      result.backend_stop_confirmed = false;
      result.error = core::ErrorDTO{
          std::string(core::kErrorMotionInterruptBackendStopFailed),
          "no discrete-trajectory execution in flight",
          {{"reason", "controlled_stop_no_execution_in_flight"}}};
      return result;
    }
    if (profile.stop_mode == core::MotionInterruptStopMode::kHardDeceleration &&
        is_commanded_sim_mode()) {
      jtc_cancel_reference_token =
          begin_jtc_cancel_reference_authority_locked();
      jtc_cancel_reference_authority_busy = jtc_cancel_reference_token == 0;
    }
    if (jtc_cancel_reference_authority_busy) {
      result.accepted = false;
      result.backend_stop_confirmed = false;
      result.error = core::ErrorDTO{
          std::string(core::kErrorMotionInterruptBackendStopFailed),
          "another JTC cancel command-reference delegation is still active",
          {{"reason", "jtc_cancel_reference_authority_busy"},
           {"mode", "hard_deceleration"}}};
      return result;
    }
    stop_requested_ = true;
    natural_completion_stop_requested_ = false;
    pause_requested_ = false;
    stop_requested_at_ = std::chrono::steady_clock::now();
    if (active_goal_handle_) {
      active_goal_to_cancel = active_goal_handle_;
    }
  }
  const auto jtc_cancel_reference_scope = std::shared_ptr<void>(
      nullptr, [this, jtc_cancel_reference_token](void *) {
        end_jtc_cancel_reference_authority(jtc_cancel_reference_token);
      });
  (void)jtc_cancel_reference_scope;
  execution_cv_.notify_all();
  cancel_goal_if_known(active_goal_to_cancel);

  // spec/260 §5.6 hard_deceleration: native JTC deceleration path.
  // Cancelling the action goal triggers the
  // controller's `decelerate_on_cancel` profile; we wait for the
  // cancel callback to terminate (running_ flips false) and then
  // read the post-stop sample and compute joint-space stop
  // distance from the pre-trip anchor. No new goal is dispatched —
  // the controller's native deceleration IS the stop.
  if (profile.stop_mode == core::MotionInterruptStopMode::kHardDeceleration) {
    const double wait_s = controlled_stop_wait_timeout_s(
        profile, config_.stop_completion_timeout_s);
    bool stopped = false;
    std::optional<rclcpp_action::ResultCode> observed_result_code;
    double observed_progress_at_stop = 0.0;
    {
      std::unique_lock<std::mutex> lock(execution_mutex_);
      stopped = execution_cv_.wait_for(
          lock, stop_token,
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::duration<double>(wait_s)),
          [this]() { return !running_ || terminal_; });
      observed_result_code = last_action_result_code_;
    }
    if (stop_token.stop_requested()) {
      return cancelled_result("hard_deceleration_wait");
    }
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      observed_progress_at_stop = snapshot_.progress;
    }
    const auto cancel_end_ns = std::chrono::steady_clock::now();
    result.confirmation_latency_ns = std::max<std::int64_t>(
        0, std::chrono::duration_cast<std::chrono::nanoseconds>(cancel_end_ns -
                                                                cancel_start_ns)
               .count());
    if (!stopped) {
      result.accepted = true;
      result.backend_stop_confirmed = false;
      result.error = core::ErrorDTO{
          std::string(core::kErrorMotionInterruptBackendStopFailed),
          "controller did not finish decelerate_on_cancel within the deadline",
          {{"reason", "backend_stop_not_confirmed"},
           {"wait_s", std::to_string(wait_s)},
           {"mode", "hard_deceleration"}}};
      return result;
    }

    // Proof of cancel.
    // `on_action_result` collapses SUCCEEDED + CANCELED into a single
    // "stopped" branch when `stop_requested_` is set, so the only
    // place that distinction survives is the persisted raw result
    // code. Without this check, a normal trajectory completion
    // racing with cancel would silently be reported as a successful
    // `kJtcCancelDecelerateOnCancel` outcome — the move actually
    // completed and the kernel would believe `decelerate_on_cancel`
    // ran when it did not.
    if (!observed_result_code.has_value()) {
      result.accepted = true;
      result.backend_stop_confirmed = false;
      result.error = core::ErrorDTO{
          std::string(core::kErrorMotionInterruptBackendStopFailed),
          "decelerate_on_cancel termination raced ahead of the action result "
          "callback; cancel proof is not yet available",
          {{"reason", "hard_deceleration_result_code_unavailable"},
           {"mode", "hard_deceleration"}}};
      return result;
    }
    if (*observed_result_code != rclcpp_action::ResultCode::CANCELED) {
      const char *code_name = "unknown";
      switch (*observed_result_code) {
      case rclcpp_action::ResultCode::SUCCEEDED:
        code_name = "succeeded";
        break;
      case rclcpp_action::ResultCode::ABORTED:
        code_name = "aborted";
        break;
      case rclcpp_action::ResultCode::CANCELED:
        code_name = "canceled";
        break;
      case rclcpp_action::ResultCode::UNKNOWN:
        code_name = "unknown";
        break;
      }
      result.accepted = true;
      result.backend_stop_confirmed = false;
      result.error = core::ErrorDTO{
          std::string(core::kErrorMotionInterruptBackendStopFailed),
          "active trajectory terminated with a non-CANCELED result code; "
          "decelerate_on_cancel did not actually run",
          {{"reason", "hard_deceleration_not_canceled"},
           {"observed_result_code", code_name},
           {"mode", "hard_deceleration"}}};
      return result;
    }

    // Read the post-stop sample from measured/controller feedback, not from an
    // authored trajectory. JTC owns the decelerate_on_cancel profile, so the
    // only portable terminal authority is a fresh settled observation. Waiting
    // here also prevents Composer from rendering a random early read-back pose
    // while Isaac/robot feedback is still converging after the cancel result.
    const auto settle_start = std::chrono::steady_clock::now();
    const auto settle_deadline =
        cancel_start_ns +
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(wait_s));
    const double maximum_settle_velocity_threshold_rad_s = *std::max_element(
        settle_velocity_thresholds->begin(), settle_velocity_thresholds->end());
    const auto post_stop_observation = wait_for_post_stop_observation(
        joint_names, *settle_velocity_thresholds, cancel_end_ns,
        settle_deadline, stop_token);
    if (stop_token.stop_requested()) {
      return cancelled_result("hard_deceleration_settle_wait");
    }
    const auto settle_end = std::chrono::steady_clock::now();
    const auto settle_wait_ns = std::max<std::int64_t>(
        0, std::chrono::duration_cast<std::chrono::nanoseconds>(settle_end -
                                                                settle_start)
               .count());
    result.diagnostics["controller_cancel_confirmation_latency_ns"] =
        core::interrupt_structured_number(
            static_cast<double>(result.confirmation_latency_ns));
    result.confirmation_latency_ns = std::max<std::int64_t>(
        0, std::chrono::duration_cast<std::chrono::nanoseconds>(settle_end -
                                                                cancel_start_ns)
               .count());
    if (!post_stop_observation.has_value() ||
        post_stop_observation->positions.size() != joint_names.size()) {
      result.accepted = true;
      result.backend_stop_confirmed = false;
      result.error = core::ErrorDTO{
          std::string(core::kErrorMotionInterruptStopStateInvalid),
          "controller finished decelerate_on_cancel but no fresh post-stop "
          "sample is available",
          {{"reason", "controlled_stop_post_hold_state_unavailable"},
           {"settle_wait_ns", std::to_string(settle_wait_ns)},
           {"mode", "hard_deceleration"}}};
      return result;
    }
    const auto &post_stop_positions = post_stop_observation->positions;
    const auto &post_stop_velocities = post_stop_observation->velocities;
    const auto &post_stop_source = post_stop_observation->source;
    result.diagnostics["post_stop_sample_source"] =
        core::interrupt_structured_string(post_stop_source);
    result.diagnostics["post_stop_velocity_available"] =
        core::interrupt_structured_bool(
            post_stop_observation->velocity_available);
    result.diagnostics["post_stop_at_rest"] =
        core::interrupt_structured_bool(post_stop_observation->at_rest);
    result.diagnostics["post_stop_max_abs_velocity_rad_s"] =
        core::interrupt_structured_number(
            post_stop_observation->max_abs_velocity_rad_s);
    result.diagnostics["max_settle_velocity_threshold_rad_s"] =
        core::interrupt_structured_number(
            maximum_settle_velocity_threshold_rad_s);
    result.diagnostics["settle_velocity_threshold_source"] =
        core::interrupt_structured_string(
            profile.per_joint_settle_velocity_threshold.has_value()
                ? "profile_per_joint_with_retained_defaults"
                : "retained_default_per_joint");
    result.diagnostics["post_stop_settle_wait_ns"] =
        core::interrupt_structured_number(static_cast<double>(settle_wait_ns));
    if (!post_stop_observation->velocity_available) {
      result.accepted = true;
      result.backend_stop_confirmed = false;
      result.error = core::ErrorDTO{
          std::string(core::kErrorMotionInterruptStopStateInvalid),
          "controller finished decelerate_on_cancel but no measured velocity "
          "sample is available to certify settle",
          {{"reason", "hard_deceleration_settle_velocity_unavailable"},
           {"sample_source", post_stop_source},
           {"settle_wait_ns", std::to_string(settle_wait_ns)},
           {"mode", "hard_deceleration"}}};
      return result;
    }
    if (!post_stop_observation->at_rest) {
      result.accepted = true;
      result.backend_stop_confirmed = false;
      result.error = core::ErrorDTO{
          std::string(core::kErrorMotionInterruptBackendStopFailed),
          "controller finished decelerate_on_cancel but measured velocity did "
          "not settle before the deadline",
          {{"reason", "hard_deceleration_not_settled"},
           {"sample_source", post_stop_source},
           {"measured_max_abs_velocity_rad_s",
            format_double(post_stop_observation->max_abs_velocity_rad_s)},
           {"max_settle_velocity_threshold_rad_s",
            format_double(maximum_settle_velocity_threshold_rad_s)},
           {"settle_wait_ns", std::to_string(settle_wait_ns)},
           {"mode", "hard_deceleration"}}};
      return result;
    }

    // Off-path stop distance: joint-space euclidean norm of the
    // delta between the pre-cancel anchor and the post-stop sample.
    // This is the spec/210 §3.11 `stop_distance_rad` evidence the
    // signal_trip outcome requires for `kHardDeceleration` runtime
    // qualification.
    double sum_sq = 0.0;
    for (std::size_t i = 0; i < joint_names.size(); ++i) {
      const double delta = post_stop_positions[i] - observed_positions[i];
      sum_sq += delta * delta;
    }
    result.stop_distance_rad = std::sqrt(sum_sq);

    // Publish the snapshot with the trajectory progress observed at
    // the moment of cancel,
    // NOT progress=1.0. The trajectory did not reach its planned
    // endpoint — `decelerate_on_cancel` stopped it mid-flight, and
    // an off-path interrupt with progress=1.0 would falsely signal
    // trajectory completion to downstream stop-snapshot consumers.
    // End the controller-reference delegation before publishing the settled
    // terminal authority. end_jtc... serializes with callbacks, so no late
    // reference can overwrite this final measured stop position.
    end_jtc_cancel_reference_authority(jtc_cancel_reference_token);
    const auto terminal_generation =
        running_generation_.load(std::memory_order_acquire);
    std::vector<double> terminal_positions = post_stop_positions;
    std::vector<double> terminal_velocities = post_stop_velocities;
    std::int64_t terminal_scene_source_ns =
        post_stop_observation->received_timestamp_ns;
    if (uses_authored_backend_state()) {
      const auto commanded_terminal = get_execution_snapshot();
      terminal_positions = commanded_terminal.joint_positions_rad;
      terminal_velocities = commanded_terminal.joint_velocities_rad_s;
      terminal_scene_source_ns = commanded_terminal.source_timestamp_ns;
      (void)publish_snapshot(
          core::ExecutionState::kStopped, observed_progress_at_stop,
          terminal_positions, std::nullopt, "", terminal_generation,
          SnapshotPublicationAuthority::kBackendAuthored,
          terminal_velocities.empty() ? nullptr : &terminal_velocities);
    } else {
      (void)publish_observed_snapshot(
          core::ExecutionState::kStopped, observed_progress_at_stop,
          terminal_positions, terminal_velocities,
          post_stop_observation->received_timestamp_ns, std::nullopt,
          terminal_generation);
    }
    project_terminal_scene_positions(terminal_positions,
                                     terminal_scene_source_ns);

    result.accepted = true;
    result.backend_stop_confirmed = true;
    result.stop_command_source =
        core::MotionInterruptStopCommandSource::kJtcCancelDecelerateOnCancel;
    return result;
  }

  result.accepted = false;
  result.backend_stop_confirmed = false;
  result.error = core::ErrorDTO{
      std::string(core::kErrorMotionInterruptBackendStopFailed),
      "requested stop_mode reached an unsupported ros2_control stop path",
      {{"reason", "stop_mode_not_supported_by_backend"}}};
  return result;
}

core::PauseResultDTO Ros2ControlExecutionBackend::pause() {
  core::ExecutionSnapshot snapshot;
  GoalHandleFJT::SharedPtr goal_to_cancel;
  bool cancel_requested = false;
  bool resync_required = false;
  std::uint64_t publication_generation = 0;
  {
    std::lock_guard<std::mutex> lock(execution_mutex_);
    resync_required = resync_required_;
    if (resync_required_) {
      snapshot = get_execution_snapshot();
    } else if (!running_ && terminal_) {
      return core::PauseResultDTO{false,
                                  make_error(core::ErrorCode::kInvalidRequest,
                                             "No execution in flight")};
    } else {
      pause_requested_ = true;
      stop_requested_ = false;
      natural_completion_stop_requested_ = false;
      stop_requested_at_ = std::chrono::steady_clock::now();
      cancel_requested = static_cast<bool>(active_goal_handle_);
      if (cancel_requested) {
        goal_to_cancel = active_goal_handle_;
        snapshot = get_execution_snapshot();
      } else {
        set_running_locked(false, ProgressRetirement::kRetireImmediately,
                           ResumeAuthorityRetention::kPreserve);
        terminal_ = true;
        snapshot = get_execution_snapshot();
      }
      publication_generation =
          running_generation_.load(std::memory_order_acquire);
    }
  }
  if (resync_required) {
    return core::PauseResultDTO{
        false,
        operator_intervention_error(core::ErrorCode::kPauseFailed,
                                    "ros2_control execution backend requires "
                                    "observed-state resync before reuse",
                                    "pause_resync_required", snapshot.state)};
  }
  execution_cv_.notify_all();
  cancel_goal_if_known(goal_to_cancel);
  const auto state = cancel_requested ? core::ExecutionState::kStopping
                                      : core::ExecutionState::kPaused;
  if (uses_authored_backend_state()) {
    publish_snapshot(state, snapshot.progress, snapshot.joint_positions_rad);
  } else {
    (void)publish_observed_metadata(state, snapshot.progress, std::nullopt,
                                    publication_generation);
  }
  cancel_all_gripper_commands();
  execution_cv_.notify_all();
  return core::PauseResultDTO{true, std::nullopt};
}

core::ExecutionHandle
Ros2ControlExecutionBackend::resume(const core::TrajectoryDTO &trajectory) {
  std::optional<DispatchAuthorityPin> resumable_environment;
  {
    std::lock_guard<std::mutex> lock(execution_mutex_);
    resumable_environment = resumable_dispatch_authority_;
  }
  if (resumable_environment.has_value()) {
    // plan_resume_trajectory() captured the current environment after its
    // observed-state barrier. Replace that provisional pin with the original
    // dispatch environment: the final linearization check will admit only if
    // every startup epoch still matches. A restarted component can therefore
    // never inherit a paused execution.
    std::lock_guard<std::mutex> lock(dispatch_admission_mutex_);
    planning_environment_authority_ = std::move(resumable_environment);
  }
  return execute_trajectory(trajectory);
}

core::ResetResultDTO Ros2ControlExecutionBackend::reset() {
  const auto publish_reset_transition =
      [this](core::ExecutionState state, const core::ExecutionSnapshot &current,
             std::optional<core::ErrorDTO> error) {
        if (uses_authored_backend_state()) {
          (void)publish_snapshot(state, current.progress,
                                 current.joint_positions_rad, std::move(error));
          return;
        }
        std::lock_guard<std::mutex> execution_lock(execution_mutex_);
        (void)publish_observed_metadata(
            state, current.progress, std::move(error),
            running_generation_.load(std::memory_order_acquire));
      };
  GoalHandleFJT::SharedPtr managed_fjt_goal_to_cancel;
  {
    std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
    managed_fjt_goal_to_cancel = managed_fjt_goal_;
    managed_fjt_goal_.reset();
    ++managed_fjt_generation_;
    forced_managed_stop_execution_generation_.reset();
    forced_managed_stop_generation_.reset();
    managed_fjt_terminal_goal_id_.reset();
    managed_fjt_terminal_result_received_ = true;
    managed_fjt_terminal_result_success_ = false;
    managed_fjt_terminal_error_ =
        make_error(core::ErrorCode::kStopFailed,
                   "managed controlled stop was preempted by backend reset",
                   {{"reason", "managed_stop_reset"}});
    managed_fjt_cv_.notify_all();
  }
  cancel_goal_if_known(managed_fjt_goal_to_cancel);

  GoalHandleFJT::SharedPtr active_goal_to_cancel;
  bool resync_required = false;
  {
    std::lock_guard<std::mutex> lock(execution_mutex_);
    resync_required = resync_required_;
    if (!resync_required && active_goal_handle_) {
      active_goal_to_cancel = active_goal_handle_;
    }
    if (!resync_required) {
      set_running_locked(false);
      terminal_ = true;
      stop_requested_ = false;
      natural_completion_stop_requested_ = false;
      pause_requested_ = false;
      stop_requested_at_.reset();
      controlled_stop_preempt_in_progress_ = false;
      controlled_stop_command_reference_.reset();
      active_goal_handle_.reset();
      active_goal_id_.reset();
      active_trajectory_.reset();
      active_trajectory_time_s_ = 0.0;
      active_runtime_velocity_scale_ = 1.0;
    }
  }
  execution_cv_.notify_all();
  cancel_goal_if_known(active_goal_to_cancel);
  if (resync_required) {
    auto resync_result = resync_after_intervention_if_needed("reset_resync");
    if (!resync_result.ok()) {
      const auto snapshot = get_execution_snapshot();
      publish_reset_transition(core::ExecutionState::kInterventionRequired,
                               snapshot, resync_result.error());
      return core::ResetResultDTO{false, resync_result.error()};
    }
    return core::ResetResultDTO{true, std::nullopt};
  }
  cancel_all_gripper_commands();
  execution_cv_.notify_all();
  if (monitor_thread_.joinable() && monitor_active_.load()) {
    std::unique_lock<std::mutex> lock(execution_mutex_);
    const bool monitor_stopped = execution_cv_.wait_for(
        lock, std::chrono::duration<double>(config_.stop_completion_timeout_s),
        [this]() { return !monitor_active_.load(); });
    if (!monitor_stopped) {
      lock.unlock();
      const auto snapshot = get_execution_snapshot();
      {
        std::lock_guard<std::mutex> exec_lock(execution_mutex_);
        resync_required_ = true;
        set_running_locked(false);
        terminal_ = true;
        natural_completion_stop_requested_ = false;
        controlled_stop_preempt_in_progress_ = false;
        controlled_stop_command_reference_.reset();
        active_goal_handle_.reset();
        active_goal_id_.reset();
        active_trajectory_.reset();
        active_trajectory_time_s_ = 0.0;
        active_runtime_velocity_scale_ = 1.0;
      }
      const auto error = operator_intervention_error(
          core::ErrorCode::kResetFailed,
          "Timed out waiting for ros2_control monitor thread to stop",
          "reset_monitor_join_timeout", snapshot.state);
      publish_reset_transition(core::ExecutionState::kInterventionRequired,
                               snapshot, error);
      return core::ResetResultDTO{false, error};
    }
  }
  if (monitor_thread_.joinable()) {
    monitor_thread_.join();
  }
  if (is_commanded_sim_mode()) {
    auto reset_result = command_sim_initial_state("reset_commanded_sim");
    if (!reset_result.ok()) {
      const auto snapshot = get_execution_snapshot();
      auto error = reset_result.error();
      publish_reset_transition(core::ExecutionState::kInterventionRequired,
                               snapshot, error);
      return core::ResetResultDTO{false, error};
    }
    return core::ResetResultDTO{true, std::nullopt};
  }
  std::unique_lock<std::mutex> scene_projection_barrier(
      scene_projection_barrier_mutex_);
  auto scene_projection_suspension = scene_projection_worker_->suspend();
  if (!scene_projection_worker_->drain_for(kSceneProjectionBarrierTimeout)) {
    const auto snapshot = get_execution_snapshot();
    const auto error = operator_intervention_error(
        core::ErrorCode::kResetFailed,
        "Timed out fencing pre-reset asynchronous scene projection",
        "reset_scene_projection_drain_timeout", snapshot.state);
    publish_reset_transition(core::ExecutionState::kInterventionRequired,
                             snapshot, error);
    return core::ResetResultDTO{false, error};
  }
  if (uses_authored_backend_state()) {
    publish_snapshot(core::ExecutionState::kIdle, 0.0, {});
  } else {
    std::lock_guard<std::mutex> execution_lock(execution_mutex_);
    if (!running_) {
      (void)publish_observed_metadata(
          core::ExecutionState::kIdle, 0.0, std::nullopt,
          running_generation_.load(std::memory_order_acquire));
    }
  }
  return core::ResetResultDTO{true, std::nullopt};
}

core::ExecutionSnapshot
Ros2ControlExecutionBackend::get_execution_snapshot() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  auto snapshot = snapshot_;
  if (!uses_authored_backend_state() && !snapshot.state_source.empty()) {
    const auto now_ns = clock_.now();
    snapshot.source_age_ns = now_ns >= snapshot.source_timestamp_ns
                                 ? now_ns - snapshot.source_timestamp_ns
                                 : 0;
  }
  return snapshot;
}

std::optional<double>
position_for_joint(const sensor_msgs::msg::JointState &msg,
                   const std::string &joint_name);

void Ros2ControlExecutionBackend::configure_gripper_action(
    std::string gripper_id, Ros2GripperActionConfig config) {
  if (gripper_id.empty()) {
    throw std::invalid_argument("gripper_id must not be empty");
  }
  if (config.transport_kind.empty()) {
    config.transport_kind = "ros_gripper_command";
  }
  if (config.transport_kind != "ros_gripper_command" &&
      config.transport_kind != "isaac_joint_state_effort") {
    throw std::invalid_argument(
        "gripper transport_kind must be ros_gripper_command or "
        "isaac_joint_state_effort");
  }
  if (config.transport_kind == "ros_gripper_command" &&
      config.action_name.empty()) {
    throw std::invalid_argument(
        "gripper action_name must not be empty for ros_gripper_command");
  }
  if (config.transport_kind == "isaac_joint_state_effort") {
    if (config.command_topic.empty()) {
      throw std::invalid_argument("gripper command_topic must not be empty for "
                                  "isaac_joint_state_effort");
    }
    if (config.command_joint_names.empty() && config.joint_name.empty()) {
      throw std::invalid_argument(
          "gripper joint_name or command_joint_names is required for "
          "isaac_joint_state_effort");
    }
    if (!std::isfinite(config.open_position) ||
        !std::isfinite(config.closed_position) ||
        config.open_position == config.closed_position ||
        !std::isfinite(config.open_effort) ||
        !std::isfinite(config.close_effort)) {
      throw std::invalid_argument(
          "isaac_joint_state_effort gripper positions and efforts are invalid");
    }
  }
  if (!std::isfinite(config.state_freshness_timeout_s) ||
      !std::isfinite(config.wait_for_action_server_timeout_s) ||
      !std::isfinite(config.result_timeout_s) ||
      config.state_freshness_timeout_s < 0.0 ||
      config.wait_for_action_server_timeout_s < 0.0 ||
      config.result_timeout_s < 0.0) {
    throw std::invalid_argument("gripper action timing config is invalid");
  }
  if (config.completion_policy != "position_reached" &&
      config.completion_policy != "driver_result" &&
      config.completion_policy != "dwell_elapsed") {
    throw std::invalid_argument(
        "gripper completion_policy must be position_reached, driver_result, "
        "or dwell_elapsed");
  }
  if (config.group_id.empty() != config.joint_name.empty()) {
    throw std::invalid_argument(
        "gripper group_id and joint_name must be configured together");
  }
  std::lock_guard<std::mutex> ros_configuration_lock(ros_configuration_mutex_);
  if (ros_configuration_complete_) {
    throw std::logic_error(
        "gripper transports must be configured before the ROS executor starts");
  }
  auto runtime = std::make_shared<GripperRuntime>();
  runtime->config = std::move(config);
  if (runtime->config.transport_kind == "ros_gripper_command") {
    runtime->action_client = rclcpp_action::create_client<GripperCommand>(
        node_, runtime->config.action_name);
  } else {
    runtime->joint_state_pub =
        node_->create_publisher<sensor_msgs::msg::JointState>(
            runtime->config.command_topic, rclcpp::SystemDefaultsQoS());
    if (!runtime->config.state_topic.empty()) {
      const std::weak_ptr<GripperRuntime> weak_runtime = runtime;
      runtime->joint_state_sub =
          node_->create_subscription<sensor_msgs::msg::JointState>(
              runtime->config.state_topic, rclcpp::SystemDefaultsQoS(),
              [weak_runtime](
                  const sensor_msgs::msg::JointState::SharedPtr msg) {
                const auto runtime = weak_runtime.lock();
                if (!runtime || !msg) {
                  return;
                }
                std::string observed_joint = runtime->config.joint_name;
                if (observed_joint.empty() &&
                    !runtime->config.command_joint_names.empty()) {
                  observed_joint = runtime->config.command_joint_names.front();
                }
                if (observed_joint.empty()) {
                  return;
                }
                const auto position = position_for_joint(*msg, observed_joint);
                if (!position.has_value()) {
                  return;
                }
                std::lock_guard<std::mutex> lock(runtime->mutex);
                runtime->observed_position = *position;
                runtime->observed_at = std::chrono::steady_clock::now();
                ++runtime->revision;
              });
    }
  }
  runtime->generation =
      next_gripper_generation_.fetch_add(1, std::memory_order_relaxed);
  if (!runtime->config.group_id.empty()) {
    const std::weak_ptr<GripperRuntime> weak_runtime = runtime;
    runtime->scene_projection_slot =
        scene_projection_worker_->register_joint_slot(
            runtime->config.group_id, runtime->config.joint_name,
            [this, weak_runtime](
                const detail::SceneProjectionCompletion &completion) {
              on_gripper_scene_projection_complete(weak_runtime, completion);
            });
  }
  std::shared_ptr<GripperRuntime> replaced_runtime;
  {
    std::lock_guard<std::mutex> authority_lock(gripper_authority_mutex_);
    std::lock_guard<std::mutex> lock(gripper_map_mutex_);
    const auto existing = grippers_.find(gripper_id);
    if (existing != grippers_.end()) {
      replaced_runtime = existing->second;
    }
    grippers_[std::move(gripper_id)] = std::move(runtime);
  }
  if (replaced_runtime) {
    GoalHandleGripper::SharedPtr goal_handle;
    rclcpp_action::Client<GripperCommand>::SharedPtr action_client;
    std::optional<core::ScheduleHandle> lifecycle_handle;
    {
      std::lock_guard<std::mutex> lock(replaced_runtime->mutex);
      goal_handle = replaced_runtime->active_goal_handle;
      action_client = replaced_runtime->action_client;
      lifecycle_handle = replaced_runtime->lifecycle_handle;
      replaced_runtime->lifecycle_handle.reset();
      ++replaced_runtime->lifecycle_schedule_generation;
      replaced_runtime->generation =
          next_gripper_generation_.fetch_add(1, std::memory_order_relaxed);
      replaced_runtime->active_goal_handle.reset();
      replaced_runtime->executing = false;
      replaced_runtime->goal_accepted = false;
      replaced_runtime->cancel_pending = true;
      replaced_runtime->state = "idle";
      ++replaced_runtime->revision;
    }
    if (lifecycle_handle.has_value()) {
      try {
        scheduler_.cancel(*lifecycle_handle);
      } catch (...) {
        // Generation fencing has already made the callback inert. Lifecycle
        // invalidation is noexcept and remains authoritative even when eager
        // scheduler cleanup fails.
      }
    }
    if (action_client && goal_handle) {
      try {
        (void)action_client->async_cancel_goal(goal_handle);
      } catch (const std::exception &exc) {
        RCLCPP_WARN(node_->get_logger(),
                    "Failed to cancel replaced gripper action goal: %s",
                    exc.what());
      }
    }
  }
}

void publish_isaac_joint_state_gripper_command(
    const rclcpp::Node::SharedPtr &node, const Ros2GripperActionConfig &config,
    const rclcpp::Publisher<sensor_msgs::msg::JointState>::SharedPtr &publisher,
    double target_position) {
  if (!publisher) {
    throw std::runtime_error(
        "Isaac gripper command publisher is not configured");
  }
  std::vector<std::string> joint_names = config.command_joint_names;
  if (joint_names.empty()) {
    joint_names.push_back(config.joint_name);
  }
  const bool closing = std::abs(target_position - config.closed_position) <=
                       std::abs(target_position - config.open_position);
  const double effort = closing ? config.close_effort : config.open_effort;

  sensor_msgs::msg::JointState msg;
  msg.header.stamp = node->now();
  msg.name = joint_names;
  if (config.publish_nominal_position) {
    msg.position.assign(joint_names.size(), target_position);
  }
  msg.effort.assign(joint_names.size(), effort);
  publisher->publish(msg);
}

std::optional<double>
position_for_joint(const sensor_msgs::msg::JointState &msg,
                   const std::string &joint_name) {
  for (std::size_t i = 0; i < msg.name.size(); ++i) {
    if (msg.name[i] == joint_name && i < msg.position.size()) {
      return std::isfinite(msg.position[i])
                 ? std::optional<double>{msg.position[i]}
                 : std::nullopt;
    }
  }
  return std::nullopt;
}

void Ros2ControlExecutionBackend::on_gripper_feedback(
    const std::shared_ptr<GripperRuntime> &runtime, std::uint64_t command_id,
    std::uint64_t generation, GoalHandleGripper::SharedPtr goal_handle,
    const std::shared_ptr<const GripperCommand::Feedback> feedback) {
  if (!runtime || !feedback) {
    return;
  }
  std::optional<double> feedback_position;
  bool reached_goal = false;
  std::optional<core::ScheduleHandle> lifecycle_handle;
  rclcpp_action::Client<GripperCommand>::SharedPtr action_client;
  {
    std::lock_guard<std::mutex> lock(runtime->mutex);
    if (runtime->command_id != command_id ||
        runtime->generation != generation || runtime->externally_failed) {
      return;
    }
    if (!runtime->executing) {
      return;
    }
    if (!std::isfinite(feedback->position)) {
      lifecycle_handle = runtime->lifecycle_handle;
      runtime->lifecycle_handle.reset();
      ++runtime->lifecycle_schedule_generation;
      action_client = runtime->action_client;
      if (!goal_handle) {
        goal_handle = runtime->active_goal_handle;
      }
      runtime->active_goal_handle.reset();
      runtime->state = "failed";
      runtime->executing = false;
      runtime->goal_accepted = false;
      runtime->cancel_pending = true;
      runtime->externally_failed = true;
      runtime->error = "Gripper action feedback position is not finite";
      runtime->reason_code = ros2_control_fault_reason::kGripperStateNotFinite;
      runtime->projection_state = core::GripperProjectionState::kNotRequired;
      runtime->projection_reason.clear();
    } else {
      runtime->current_position = feedback->position;
      runtime->progress =
          clamp_progress(static_cast<double>(std::max<core::Duration>(
                             0, clock_.now() - runtime->started_at_ns)) /
                         static_cast<double>(std::max<core::Duration>(
                             1, gripper_duration_ns(runtime->duration_s))));
      feedback_position = feedback->position;
      reached_goal = feedback->reached_goal;
      if (reached_goal) {
        runtime->state = "completed";
        runtime->executing = false;
        runtime->goal_accepted = false;
        runtime->cancel_pending = false;
        runtime->active_goal_handle.reset();
        lifecycle_handle = runtime->lifecycle_handle;
        runtime->lifecycle_handle.reset();
        ++runtime->lifecycle_schedule_generation;
        runtime->progress = 1.0;
        runtime->error.clear();
        runtime->reason_code.clear();
      }
    }
    ++runtime->revision;
  }
  if (lifecycle_handle.has_value()) {
    scheduler_.cancel(*lifecycle_handle);
  }
  if (action_client && goal_handle) {
    try {
      (void)action_client->async_cancel_goal(goal_handle);
    } catch (const std::exception &exc) {
      RCLCPP_WARN(node_->get_logger(),
                  "Failed to cancel gripper goal after non-finite feedback: %s",
                  exc.what());
    }
  }
  if (feedback_position.has_value() &&
      (!is_commanded_sim_mode() || reached_goal)) {
    bool retry_projection = false;
    {
      std::lock_guard<std::mutex> lock(runtime->mutex);
      if (runtime->command_id == command_id &&
          runtime->generation == generation) {
        const bool queued = schedule_gripper_scene_projection_locked(
            runtime, *feedback_position, reached_goal);
        retry_projection = reached_goal && !queued;
        ++runtime->revision;
      }
    }
    if (retry_projection) {
      (void)schedule_gripper_lifecycle(runtime, command_id, generation,
                                       clock_.now() + 20'000'000);
    }
  }
}

void Ros2ControlExecutionBackend::on_gripper_result(
    const std::shared_ptr<GripperRuntime> &runtime, std::uint64_t command_id,
    std::uint64_t generation, const GoalHandleGripper::WrappedResult &result) {
  if (!runtime) {
    return;
  }
  Ros2GripperActionConfig runtime_config;
  std::optional<double> result_position;
  std::optional<core::ScheduleHandle> lifecycle_handle;
  {
    std::lock_guard<std::mutex> lock(runtime->mutex);
    if (runtime->command_id != command_id ||
        runtime->generation != generation) {
      return;
    }
    if (runtime->externally_failed) {
      return;
    }
    runtime_config = runtime->config;
    runtime->active_goal_handle.reset();
    runtime->goal_accepted = false;
    runtime->cancel_pending = false;
    runtime->executing = false;
    lifecycle_handle = runtime->lifecycle_handle;
    runtime->lifecycle_handle.reset();
    ++runtime->lifecycle_schedule_generation;
    const bool result_position_not_finite =
        result.result && !std::isfinite(result.result->position);
    if (result.result && !result_position_not_finite) {
      runtime->current_position = result.result->position;
      result_position = result.result->position;
    }
    if (result_position_not_finite) {
      runtime->state = "failed";
      runtime->externally_failed = true;
      runtime->error = "Gripper action result position is not finite";
      runtime->reason_code = ros2_control_fault_reason::kGripperStateNotFinite;
      runtime->projection_state = core::GripperProjectionState::kNotRequired;
      runtime->projection_reason.clear();
    } else {
      switch (result.code) {
      case rclcpp_action::ResultCode::SUCCEEDED:
        if (runtime_config.completion_policy == "driver_result" ||
            runtime_config.completion_policy == "dwell_elapsed" ||
            (result.result && result.result->reached_goal)) {
          runtime->state = "completed";
          runtime->progress = 1.0;
          runtime->error.clear();
          runtime->reason_code.clear();
          if (!result_position.has_value()) {
            result_position = runtime->target_position;
          }
        } else {
          runtime->state = "failed";
          runtime->error = "Gripper action succeeded without reaching the goal";
          runtime->reason_code =
              ros2_control_fault_reason::kGripperResultFailed;
        }
        break;
      case rclcpp_action::ResultCode::ABORTED:
        runtime->state = "failed";
        runtime->error = "Gripper action aborted";
        runtime->reason_code = ros2_control_fault_reason::kGripperGoalAborted;
        break;
      case rclcpp_action::ResultCode::CANCELED:
        runtime->state = "idle";
        runtime->error.clear();
        runtime->reason_code.clear();
        break;
      default:
        runtime->state = "failed";
        runtime->error = "Gripper action returned an unknown result code";
        runtime->reason_code = ros2_control_fault_reason::kGripperResultFailed;
        break;
      }
    }
    ++runtime->revision;
  }
  if (lifecycle_handle.has_value()) {
    scheduler_.cancel(*lifecycle_handle);
  }
  if (result_position.has_value()) {
    bool retry_projection = false;
    {
      std::lock_guard<std::mutex> lock(runtime->mutex);
      if (runtime->command_id == command_id &&
          runtime->generation == generation) {
        const bool terminal = runtime->state == "completed";
        const bool queued = schedule_gripper_scene_projection_locked(
            runtime, *result_position, terminal);
        retry_projection = terminal && !queued;
        ++runtime->revision;
      }
    }
    if (retry_projection) {
      (void)schedule_gripper_lifecycle(runtime, command_id, generation,
                                       clock_.now() + 20'000'000);
    }
  }
}

void Ros2ControlExecutionBackend::cancel_all_gripper_commands() {
  invalidate_gripper_commands(core::GripperInvalidationReason::kStop);
}

void Ros2ControlExecutionBackend::invalidate_gripper_commands(
    core::GripperInvalidationReason /*reason*/) noexcept {
  std::vector<std::shared_ptr<GripperRuntime>> runtimes;
  {
    std::lock_guard<std::mutex> authority_lock(gripper_authority_mutex_);
    gripper_admission_blocked_ = true;
    ++gripper_authority_generation_;
    std::lock_guard<std::mutex> lock(gripper_map_mutex_);
    for (const auto &[_, runtime] : grippers_) {
      runtimes.push_back(runtime);
    }
  }
  for (auto &runtime : runtimes) {
    GoalHandleGripper::SharedPtr goal_handle;
    rclcpp_action::Client<GripperCommand>::SharedPtr action_client;
    std::optional<core::ScheduleHandle> lifecycle_handle;
    {
      std::lock_guard<std::mutex> lock(runtime->mutex);
      goal_handle = runtime->active_goal_handle;
      action_client = runtime->action_client;
      lifecycle_handle = runtime->lifecycle_handle;
      runtime->lifecycle_handle.reset();
      ++runtime->lifecycle_schedule_generation;
      runtime->generation =
          next_gripper_generation_.fetch_add(1, std::memory_order_relaxed);
      runtime->executing = false;
      runtime->goal_accepted = false;
      runtime->cancel_pending = true;
      runtime->active_goal_handle.reset();
      runtime->state = "idle";
      runtime->progress = 0.0;
      runtime->evidence.clear();
      runtime->evidence_state.clear();
      runtime->externally_failed = false;
      runtime->error.clear();
      runtime->reason_code.clear();
      runtime->projection_state = core::GripperProjectionState::kNotRequired;
      runtime->projection_reason.clear();
      ++runtime->revision;
    }
    if (lifecycle_handle.has_value()) {
      scheduler_.cancel(*lifecycle_handle);
    }
    if (action_client && goal_handle) {
      try {
        (void)action_client->async_cancel_goal(goal_handle);
      } catch (const std::exception &exc) {
        RCLCPP_WARN(node_->get_logger(),
                    "Failed to cancel gripper action goal: %s", exc.what());
      }
    }
  }
}

void Ros2ControlExecutionBackend::authorize_gripper_commands() noexcept {
  std::lock_guard<std::mutex> lock(gripper_authority_mutex_);
  if (!gripper_admission_blocked_) {
    return;
  }
  ++gripper_authority_generation_;
  gripper_admission_blocked_ = false;
}

Ros2GripperTransportReadinessSnapshot
Ros2ControlExecutionBackend::gripper_transport_readiness(
    const std::string &gripper_id) const {
  Ros2GripperTransportReadinessSnapshot result;
  result.command_gate_open =
      config_.command_gate_topic.empty() ||
      command_gate_enabled_.load(std::memory_order_acquire);
  if (gripper_id.empty()) {
    result.reason_codes.push_back("gripper_identity_missing");
    return result;
  }

  std::shared_ptr<GripperRuntime> runtime;
  {
    std::lock_guard<std::mutex> map_lock(gripper_map_mutex_);
    const auto found = grippers_.find(gripper_id);
    if (found == grippers_.end()) {
      result.reason_codes.push_back("gripper_transport_not_configured");
      return result;
    }
    runtime = found->second;
  }

  Ros2GripperActionConfig runtime_config;
  rclcpp_action::Client<GripperCommand>::SharedPtr action_client;
  rclcpp::Publisher<sensor_msgs::msg::JointState>::SharedPtr command_publisher;
  std::optional<std::chrono::steady_clock::time_point> observed_at;
  {
    std::lock_guard<std::mutex> runtime_lock(runtime->mutex);
    runtime_config = runtime->config;
    action_client = runtime->action_client;
    command_publisher = runtime->joint_state_pub;
    observed_at = runtime->observed_at;
  }
  result.transport_kind = runtime_config.transport_kind;
  result.configured = true;

  try {
    if (runtime_config.transport_kind == "ros_gripper_command") {
      result.command_path_ready =
          action_client && action_client->action_server_is_ready();
      result.feedback_path_ready = result.command_path_ready;
      if (!result.command_path_ready) {
        result.reason_codes.push_back("gripper_action_server_unavailable");
      }
    } else if (runtime_config.transport_kind == "isaac_joint_state_effort") {
      result.command_path_ready =
          command_publisher && command_publisher->get_subscription_count() > 0U;
      if (!result.command_path_ready) {
        result.reason_codes.push_back("gripper_command_subscriber_unavailable");
      }
      if (runtime_config.state_topic.empty()) {
        result.feedback_path_ready = true;
      } else if (observed_at.has_value()) {
        const auto age = std::chrono::steady_clock::now() - *observed_at;
        const auto freshness = std::chrono::duration<double>(
            runtime_config.state_freshness_timeout_s);
        result.feedback_path_ready =
            age >= decltype(age)::zero() && age <= freshness;
      }
      if (!result.feedback_path_ready) {
        result.reason_codes.push_back("gripper_state_feedback_unavailable");
      }
    } else {
      result.reason_codes.push_back("gripper_transport_kind_invalid");
    }
  } catch (...) {
    result.command_path_ready = false;
    result.feedback_path_ready = false;
    result.reason_codes.push_back("gripper_transport_readiness_exception");
  }

  if (!result.command_gate_open) {
    result.reason_codes.push_back("gripper_command_gate_closed");
  }
  {
    std::lock_guard<std::mutex> map_lock(gripper_map_mutex_);
    const auto current = grippers_.find(gripper_id);
    if (current == grippers_.end() || current->second != runtime) {
      result.configured = false;
      result.command_path_ready = false;
      result.feedback_path_ready = false;
      result.reason_codes.push_back("gripper_transport_replaced");
    }
  }
  std::sort(result.reason_codes.begin(), result.reason_codes.end());
  result.reason_codes.erase(
      std::unique(result.reason_codes.begin(), result.reason_codes.end()),
      result.reason_codes.end());
  result.ready = result.configured && result.command_path_ready &&
                 result.feedback_path_ready && result.command_gate_open &&
                 result.reason_codes.empty();
  return result;
}

core::Result<void> Ros2ControlExecutionBackend::validate_gripper_command(
    const core::GripperCommandRequestDTO &request) const {
  const auto rejected = [](std::string reason, std::string message) {
    return core::Result<void>::Err(core::ErrorDTO{
        std::string(core::to_string(core::ErrorCode::kInvalidRequest)),
        std::move(message),
        {{"reason", std::move(reason)}}});
  };
  const double max_effort = request.max_effort.value_or(0.0);
  if (request.gripper_id.empty() || !std::isfinite(request.target_position) ||
      !std::isfinite(request.duration_s) || !std::isfinite(max_effort) ||
      request.duration_s < 0.0) {
    return rejected(ros2_control_fault_reason::kGripperInvalidCommand,
                    "Invalid gripper command target, duration, or max effort");
  }

  std::shared_ptr<GripperRuntime> runtime;
  {
    std::lock_guard<std::mutex> authority_lock(gripper_authority_mutex_);
    if (gripper_admission_blocked_) {
      return rejected(
          "gripper_command_not_authorized",
          "Gripper command admission is closed by the execution lifecycle");
    }
    std::lock_guard<std::mutex> map_lock(gripper_map_mutex_);
    const auto configured = grippers_.find(request.gripper_id);
    if (configured == grippers_.end()) {
      return rejected(ros2_control_fault_reason::kGripperNotConfigured,
                      "No gripper transport configured for " +
                          request.gripper_id);
    }
    runtime = configured->second;
  }

  Ros2GripperActionConfig runtime_config;
  {
    std::lock_guard<std::mutex> lock(runtime->mutex);
    runtime_config = runtime->config;
    if (runtime->executing) {
      return rejected(ros2_control_fault_reason::kGripperBusy,
                      "Gripper is already executing");
    }
    if (runtime->state == "failed" &&
        runtime->reason_code != "grasp_confirmation_lost") {
      return rejected(runtime->reason_code.empty() ? "gripper_request_failed"
                                                   : runtime->reason_code,
                      runtime->error.empty() ? "Gripper state is unavailable"
                                             : runtime->error);
    }
    if (runtime->projection_state == core::GripperProjectionState::kPending) {
      return rejected(ros2_control_fault_reason::kGripperBusy,
                      "Gripper terminal scene projection is still pending");
    }
    if (runtime->projection_state == core::GripperProjectionState::kFailed) {
      return rejected(
          "gripper_projection_failed",
          "Gripper terminal scene projection failed; reset before retrying");
    }
  }
  if ((!request.transport_kind.empty() &&
       request.transport_kind != runtime_config.transport_kind) ||
      (!request.group_id.empty() &&
       request.group_id != runtime_config.group_id) ||
      (!request.joint_name.empty() &&
       request.joint_name != runtime_config.joint_name) ||
      (!request.completion_policy.empty() &&
       request.completion_policy != runtime_config.completion_policy)) {
    return rejected(
        ros2_control_fault_reason::kInvalidGripperSemantics,
        "Gripper request semantics do not match the configured transport");
  }

  if (!runtime_config.group_id.empty() || !runtime_config.joint_name.empty()) {
    std::optional<double> start_position;
    try {
      start_position = scene_group_joint_position(runtime_config.group_id,
                                                  runtime_config.joint_name);
    } catch (const std::exception &exception) {
      return rejected("gripper_state_unavailable",
                      std::string("Cannot read gripper execution state: ") +
                          exception.what());
    } catch (...) {
      return rejected("gripper_state_unavailable",
                      "Cannot read gripper execution state");
    }
    if (!start_position.has_value()) {
      return rejected(ros2_control_fault_reason::kGripperNotConfigured,
                      "Cannot project gripper '" + request.gripper_id +
                          "' into execution-state sink group '" +
                          runtime_config.group_id + "' joint '" +
                          runtime_config.joint_name + "'");
    }
    if (!std::isfinite(*start_position)) {
      return rejected(
          ros2_control_fault_reason::kGripperStateNotFinite,
          "Configured gripper execution-state position is not finite");
    }
  }
  return core::Result<void>::Ok();
}

core::GripperCommandResultDTO Ros2ControlExecutionBackend::command_gripper(
    const core::GripperCommandRequestDTO &request) {
  const double max_effort = request.max_effort.value_or(0.0);
  if (request.gripper_id.empty() || !std::isfinite(request.target_position) ||
      !std::isfinite(request.duration_s) || !std::isfinite(max_effort) ||
      request.duration_s < 0.0) {
    return rejected_gripper_command(
        ros2_control_fault_reason::kGripperInvalidCommand,
        "Invalid gripper command target, duration, or max effort");
  }
  std::shared_ptr<GripperRuntime> runtime;
  std::uint64_t authority_generation = 0;
  {
    std::lock_guard<std::mutex> authority_lock(gripper_authority_mutex_);
    if (gripper_admission_blocked_) {
      return rejected_gripper_command(
          "gripper_command_not_authorized",
          "Gripper command admission is closed by the execution lifecycle");
    }
    authority_generation = gripper_authority_generation_;
    std::lock_guard<std::mutex> lock(gripper_map_mutex_);
    const auto it = grippers_.find(request.gripper_id);
    if (it == grippers_.end()) {
      return rejected_gripper_command(
          ros2_control_fault_reason::kGripperNotConfigured,
          "No gripper transport configured for " + request.gripper_id);
    }
    runtime = it->second;
  }
  const Ros2GripperActionConfig runtime_config = runtime->config;
  const auto runtime_is_current = [this, &request, &runtime]() {
    std::lock_guard<std::mutex> lock(gripper_map_mutex_);
    const auto current = grippers_.find(request.gripper_id);
    return current != grippers_.end() && current->second == runtime;
  };
  const auto reject_existing_state =
      [&runtime]() -> std::optional<core::GripperCommandResultDTO> {
    std::lock_guard<std::mutex> lock(runtime->mutex);
    if (runtime->executing) {
      return rejected_gripper_command(ros2_control_fault_reason::kGripperBusy,
                                      "Gripper is already executing",
                                      runtime->command_id, runtime->generation);
    }
    if (runtime->state == "failed" &&
        runtime->reason_code != "grasp_confirmation_lost") {
      return rejected_gripper_command(
          runtime->reason_code.empty() ? "gripper_request_failed"
                                       : runtime->reason_code,
          runtime->error.empty() ? "Gripper state is unavailable"
                                 : runtime->error,
          runtime->command_id, runtime->generation);
    }
    if (runtime->projection_state == core::GripperProjectionState::kPending) {
      return rejected_gripper_command(
          ros2_control_fault_reason::kGripperBusy,
          "Gripper terminal scene projection is still pending",
          runtime->command_id, runtime->generation);
    }
    if (runtime->projection_state == core::GripperProjectionState::kFailed) {
      return rejected_gripper_command(
          "gripper_projection_failed",
          "Gripper terminal scene projection failed; reset before retrying",
          runtime->command_id, runtime->generation);
    }
    return std::nullopt;
  };
  if (auto rejected = reject_existing_state()) {
    return *rejected;
  }
  if ((!request.transport_kind.empty() &&
       request.transport_kind != runtime_config.transport_kind) ||
      (!request.group_id.empty() &&
       request.group_id != runtime_config.group_id) ||
      (!request.joint_name.empty() &&
       request.joint_name != runtime_config.joint_name) ||
      (!request.completion_policy.empty() &&
       request.completion_policy != runtime_config.completion_policy)) {
    return rejected_gripper_command(
        ros2_control_fault_reason::kInvalidGripperSemantics,
        "Gripper request semantics do not match the configured transport");
  }
  std::optional<double> start_position;
  if (!runtime_config.group_id.empty() || !runtime_config.joint_name.empty()) {
    try {
      start_position = scene_group_joint_position(runtime_config.group_id,
                                                  runtime_config.joint_name);
    } catch (const std::exception &exception) {
      return rejected_gripper_command(
          "gripper_state_unavailable",
          std::string("Cannot read gripper execution state: ") +
              exception.what());
    } catch (...) {
      return rejected_gripper_command("gripper_state_unavailable",
                                      "Cannot read gripper execution state");
    }
    if (!start_position.has_value()) {
      return rejected_gripper_command(
          ros2_control_fault_reason::kGripperNotConfigured,
          "Cannot project gripper '" + request.gripper_id +
              "' into execution-state sink group '" + runtime_config.group_id +
              "' joint '" + runtime_config.joint_name + "'");
    }
    if (!std::isfinite(*start_position)) {
      return rejected_gripper_command(
          ros2_control_fault_reason::kGripperStateNotFinite,
          "Configured gripper execution-state position is not finite");
    }
  }

  if (runtime_config.transport_kind == "ros_gripper_command") {
    if (!wait_for_action_server_until(
            runtime->action_client,
            runtime->config.wait_for_action_server_timeout_s)) {
      return rejected_gripper_command(
          ros2_control_fault_reason::kGripperActionServerUnavailable,
          "Timed out waiting for gripper action server " +
              runtime->config.action_name);
    }
  } else if (!config_.command_gate_topic.empty() &&
             !command_gate_enabled_.load(std::memory_order_acquire)) {
    return rejected_gripper_command(
        ros2_control_fault_reason::kGripperGoalRejected,
        "Gripper command rejected (command gate closed: simulator reset "
        "incomplete or failed)");
  }

  const auto command_id =
      next_gripper_command_id_.fetch_add(1, std::memory_order_relaxed);
  const auto generation =
      next_gripper_generation_.fetch_add(1, std::memory_order_relaxed);
  std::optional<core::ScheduleHandle> superseded_lifecycle_handle;
  {
    std::lock_guard<std::mutex> authority_lock(gripper_authority_mutex_);
    if (gripper_admission_blocked_ ||
        gripper_authority_generation_ != authority_generation ||
        !runtime_is_current()) {
      return rejected_gripper_command(
          "gripper_command_not_authorized",
          "Gripper lifecycle authority changed during command admission");
    }
    std::lock_guard<std::mutex> lock(runtime->mutex);
    if (runtime->executing) {
      return rejected_gripper_command(ros2_control_fault_reason::kGripperBusy,
                                      "Gripper " + request.gripper_id +
                                          " is already executing",
                                      runtime->command_id, runtime->generation);
    }
    if (runtime->state == "failed" &&
        runtime->reason_code != "grasp_confirmation_lost") {
      return rejected_gripper_command(
          runtime->reason_code.empty() ? "gripper_request_failed"
                                       : runtime->reason_code,
          runtime->error.empty() ? "Gripper state is unavailable"
                                 : runtime->error,
          runtime->command_id, runtime->generation);
    }
    if (runtime->projection_state == core::GripperProjectionState::kPending) {
      return rejected_gripper_command(
          ros2_control_fault_reason::kGripperBusy,
          "Gripper terminal scene projection is still pending",
          runtime->command_id, runtime->generation);
    }
    if (runtime->projection_state == core::GripperProjectionState::kFailed) {
      return rejected_gripper_command(
          "gripper_projection_failed",
          "Gripper terminal scene projection failed; reset before retrying",
          runtime->command_id, runtime->generation);
    }
    superseded_lifecycle_handle = runtime->lifecycle_handle;
    runtime->lifecycle_handle.reset();
    ++runtime->lifecycle_schedule_generation;
    runtime->state = "executing";
    runtime->executing = true;
    runtime->command_id = command_id;
    runtime->generation = generation;
    ++runtime->revision;
    runtime->progress = 0.0;
    runtime->goal_accepted = false;
    runtime->cancel_pending = false;
    runtime->start_position = start_position;
    runtime->current_position = start_position;
    runtime->target_position = request.target_position;
    runtime->duration_s = request.duration_s;
    runtime->max_effort = request.max_effort;
    runtime->started_at_ns = clock_.now();
    runtime->deadline_at_ns.reset();
    if (runtime_config.transport_kind == "ros_gripper_command" &&
        runtime->config.result_timeout_s > 0.0) {
      runtime->deadline_at_ns = gripper_deadline(
          runtime->started_at_ns,
          gripper_duration_sum(
              gripper_duration_ns(request.duration_s),
              gripper_duration_ns(runtime->config.result_timeout_s)));
    }
    runtime->active_goal_handle.reset();
    runtime->commanded_state = request.commanded_state;
    runtime->actuation_kind = request.actuation_kind;
    runtime->state_authority = request.state_authority;
    runtime->completion_policy = request.completion_policy.empty()
                                     ? runtime_config.completion_policy
                                     : request.completion_policy;
    runtime->grasp_confirmation_policy = request.grasp_confirmation_policy;
    runtime->transport_kind = runtime_config.transport_kind;
    runtime->evidence = request.evidence;
    runtime->evidence_state = request.evidence_state;
    runtime->projection_state = runtime_config.group_id.empty()
                                    ? core::GripperProjectionState::kNotRequired
                                    : core::GripperProjectionState::kPending;
    runtime->projection_reason.clear();
    runtime->projection_retry_count = 0;
    runtime->externally_failed = false;
    runtime->error.clear();
    runtime->reason_code.clear();
  }
  if (superseded_lifecycle_handle.has_value()) {
    scheduler_.cancel(*superseded_lifecycle_handle);
  }

  if (runtime_config.transport_kind == "isaac_joint_state_effort") {
    {
      std::lock_guard<std::mutex> authority_lock(gripper_authority_mutex_);
      if (gripper_admission_blocked_ ||
          gripper_authority_generation_ != authority_generation ||
          !runtime_is_current()) {
        return rejected_gripper_command(
            "gripper_command_not_authorized",
            "Gripper lifecycle authority changed before command dispatch",
            command_id, generation);
      }
      std::lock_guard<std::mutex> lock(runtime->mutex);
      if (!runtime->executing || runtime->command_id != command_id ||
          runtime->generation != generation) {
        return rejected_gripper_command(
            "gripper_command_not_authorized",
            "Gripper command was invalidated before dispatch", command_id,
            generation);
      }
      try {
        publish_isaac_joint_state_gripper_command(node_, runtime_config,
                                                  runtime->joint_state_pub,
                                                  request.target_position);
      } catch (const std::exception &exc) {
        runtime->state = "failed";
        runtime->executing = false;
        runtime->goal_accepted = false;
        runtime->cancel_pending = false;
        runtime->error =
            std::string("Failed to publish Isaac gripper command: ") +
            exc.what();
        runtime->reason_code =
            ros2_control_fault_reason::kGripperGoalDispatchFailed;
        ++runtime->revision;
        return rejected_gripper_command(
            ros2_control_fault_reason::kGripperGoalDispatchFailed,
            runtime->error, command_id, generation);
      }
    }
    if (!schedule_gripper_lifecycle(runtime, command_id, generation,
                                    clock_.now())) {
      // The physical command was already published. Report truthful admission;
      // the owner-published status is now terminal failed (or was superseded by
      // a concurrent lifecycle transition) and must not be disguised as a
      // side-effect-free rejection.
      return {true, command_id, generation, std::nullopt};
    }
    return {true, command_id, generation, std::nullopt};
  }

  GripperCommand::Goal goal;
  goal.command.position = request.target_position;
  goal.command.max_effort = max_effort;

  rclcpp_action::Client<GripperCommand>::SendGoalOptions options;
  // Runtime state is generation-owned. Backend access is limited to cancelling
  // the owner-scheduled deadline; stop_ros() joins this callback group before
  // backend destruction completes.
  // A closed command gate makes the bridge reject every goal; name that
  // cause instead of leaving an unexplained rejection. Only when a gate is
  // configured — non-gated (real-controller) deployments must not get a
  // false gate attribution.
  const bool gate_closed_at_dispatch =
      !config_.command_gate_topic.empty() &&
      !command_gate_enabled_.load(std::memory_order_acquire);
  options.goal_response_callback =
      [this, runtime, gate_closed_at_dispatch, command_id,
       generation](const GoalHandleGripper::SharedPtr &goal_handle) {
        GoalHandleGripper::SharedPtr handle_to_cancel;
        rclcpp_action::Client<GripperCommand>::SharedPtr action_client;
        std::optional<core::ScheduleHandle> lifecycle_handle;
        {
          std::lock_guard<std::mutex> lock(runtime->mutex);
          if (runtime->command_id != command_id ||
              runtime->generation != generation) {
            handle_to_cancel = goal_handle;
            action_client = runtime->action_client;
          } else if (!goal_handle) {
            if (runtime->executing) {
              lifecycle_handle = runtime->lifecycle_handle;
              runtime->lifecycle_handle.reset();
              ++runtime->lifecycle_schedule_generation;
              runtime->state = "failed";
              runtime->executing = false;
              runtime->goal_accepted = false;
              runtime->cancel_pending = false;
              runtime->error =
                  gate_closed_at_dispatch
                      ? "Gripper action goal was rejected (command gate "
                        "closed: simulator reset incomplete or failed — run "
                        "simulation reset)"
                      : "Gripper action goal was rejected";
              runtime->reason_code =
                  ros2_control_fault_reason::kGripperGoalRejected;
              runtime->projection_state =
                  core::GripperProjectionState::kNotRequired;
              runtime->projection_reason.clear();
              ++runtime->revision;
            }
          } else if (runtime->cancel_pending || !runtime->executing) {
            lifecycle_handle = runtime->lifecycle_handle;
            runtime->lifecycle_handle.reset();
            ++runtime->lifecycle_schedule_generation;
            handle_to_cancel = goal_handle;
            action_client = runtime->action_client;
            runtime->active_goal_handle.reset();
            runtime->goal_accepted = false;
            runtime->executing = false;
            runtime->cancel_pending = false;
            runtime->state = "idle";
            runtime->error.clear();
            runtime->reason_code.clear();
            runtime->projection_state =
                core::GripperProjectionState::kNotRequired;
            runtime->projection_reason.clear();
            ++runtime->revision;
          } else {
            runtime->goal_accepted = true;
            runtime->active_goal_handle = goal_handle;
            ++runtime->revision;
          }
        }
        if (lifecycle_handle.has_value()) {
          scheduler_.cancel(*lifecycle_handle);
        }
        if (action_client && handle_to_cancel) {
          try {
            (void)action_client->async_cancel_goal(handle_to_cancel);
          } catch (const std::exception &) {
            // Reset/stop remains best-effort if the remote goal disappears
            // during negotiation; the next status/result callback will
            // reconcile state.
          }
        }
      };
  options.feedback_callback =
      [this, runtime, command_id, generation](
          GoalHandleGripper::SharedPtr goal_handle,
          const std::shared_ptr<const GripperCommand::Feedback> feedback) {
        on_gripper_feedback(runtime, command_id, generation, goal_handle,
                            feedback);
      };
  options.result_callback =
      [this, runtime, command_id,
       generation](const GoalHandleGripper::WrappedResult &result) {
        on_gripper_result(runtime, command_id, generation, result);
      };

  {
    std::lock_guard<std::mutex> authority_lock(gripper_authority_mutex_);
    if (gripper_admission_blocked_ ||
        gripper_authority_generation_ != authority_generation ||
        !runtime_is_current()) {
      return rejected_gripper_command(
          "gripper_command_not_authorized",
          "Gripper lifecycle authority changed before command dispatch",
          command_id, generation);
    }
    std::lock_guard<std::mutex> lock(runtime->mutex);
    if (!runtime->executing || runtime->command_id != command_id ||
        runtime->generation != generation) {
      return rejected_gripper_command(
          "gripper_command_not_authorized",
          "Gripper command was invalidated before dispatch", command_id,
          generation);
    }
    try {
      // rclcpp_action queues these callbacks on the executor; it does not
      // invoke them inline. Keeping the authority and runtime leaf locks
      // through this bounded enqueue makes dispatch linearizable with
      // reset/replacement.
      (void)runtime->action_client->async_send_goal(goal, options);
    } catch (const std::exception &exc) {
      runtime->state = "failed";
      runtime->executing = false;
      runtime->goal_accepted = false;
      runtime->cancel_pending = false;
      runtime->error =
          std::string("Failed to dispatch gripper action goal: ") + exc.what();
      runtime->reason_code =
          ros2_control_fault_reason::kGripperGoalDispatchFailed;
      ++runtime->revision;
      return rejected_gripper_command(
          ros2_control_fault_reason::kGripperGoalDispatchFailed,
          "Failed to dispatch gripper action goal: " + std::string(exc.what()),
          command_id, generation);
    }
  }
  if (!schedule_gripper_lifecycle(runtime, command_id, generation,
                                  clock_.now())) {
    // async_send_goal already crossed the transport boundary. Preserve the
    // admitted result and expose scheduler failure through terminal status;
    // the goal-response callback cancels a late handle for the failed owner.
    return {true, command_id, generation, std::nullopt};
  }
  return {true, command_id, generation, std::nullopt};
}

core::GripperCommandResultDTO
Ros2ControlExecutionBackend::mark_gripper_grasp_lost(
    const std::string &gripper_id, std::uint64_t expected_command_id,
    std::uint64_t expected_generation, const std::string &evidence_label,
    const std::string &message, const std::string &reason_code) {
  if (gripper_id.empty() || evidence_label.empty()) {
    return rejected_gripper_command(
        ros2_control_fault_reason::kGripperInvalidCommand,
        "gripper_id and evidence_label are required");
  }
  std::shared_ptr<GripperRuntime> runtime;
  {
    std::lock_guard<std::mutex> lock(gripper_map_mutex_);
    const auto it = grippers_.find(gripper_id);
    if (it == grippers_.end()) {
      return rejected_gripper_command(
          ros2_control_fault_reason::kGripperNotConfigured,
          "No gripper transport configured for " + gripper_id);
    }
    runtime = it->second;
  }

  GoalHandleGripper::SharedPtr goal_handle;
  rclcpp_action::Client<GripperCommand>::SharedPtr action_client;
  std::optional<core::ScheduleHandle> lifecycle_handle;
  std::uint64_t command_id = 0;
  std::uint64_t generation = 0;
  {
    std::lock_guard<std::mutex> lock(runtime->mutex);
    const bool has_command_or_status =
        runtime->state != "idle" || runtime->current_position.has_value() ||
        runtime->observed_position.has_value() || !runtime->evidence.empty() ||
        !runtime->error.empty() || !runtime->reason_code.empty();
    if (!has_command_or_status || runtime->command_id != expected_command_id ||
        runtime->generation != expected_generation) {
      return rejected_gripper_command("stale_gripper_state",
                                      "Gripper '" + gripper_id +
                                          "' command identity changed before "
                                          "grasp-loss evidence was applied");
    }
    command_id = runtime->command_id;
    generation =
        next_gripper_generation_.fetch_add(1, std::memory_order_relaxed);
    goal_handle = runtime->active_goal_handle;
    action_client = runtime->action_client;
    lifecycle_handle = runtime->lifecycle_handle;
    runtime->lifecycle_handle.reset();
    ++runtime->lifecycle_schedule_generation;
    runtime->generation = generation;
    runtime->active_goal_handle.reset();
    runtime->state = "failed";
    runtime->executing = false;
    runtime->goal_accepted = false;
    runtime->cancel_pending = false;
    runtime->externally_failed = true;
    runtime->projection_state = core::GripperProjectionState::kNotRequired;
    runtime->projection_reason.clear();
    add_unique_evidence(runtime->evidence, evidence_label);
    runtime->evidence_state = "pressure_or_part_lost";
    runtime->error =
        message.empty() ? "gripper grasp confirmation was lost" : message;
    runtime->reason_code =
        reason_code.empty() ? "grasp_confirmation_lost" : reason_code;
    ++runtime->revision;
  }

  if (lifecycle_handle.has_value()) {
    try {
      scheduler_.cancel(*lifecycle_handle);
    } catch (...) {
      // The generation changed before this eager cleanup. A queued callback
      // cannot mutate the externally failed status.
    }
  }

  if (action_client && goal_handle) {
    try {
      (void)action_client->async_cancel_goal(goal_handle);
    } catch (const std::exception &exc) {
      RCLCPP_WARN(node_->get_logger(),
                  "Failed to cancel lost-grasp gripper action goal: %s",
                  exc.what());
    }
  }
  return {true, command_id, generation, std::nullopt};
}

bool Ros2ControlExecutionBackend::schedule_gripper_lifecycle(
    const std::shared_ptr<GripperRuntime> &runtime, std::uint64_t command_id,
    std::uint64_t generation, core::TimePoint deadline_ns) {
  std::optional<core::ScheduleHandle> old_handle;
  std::uint64_t schedule_generation = 0;
  {
    std::lock_guard<std::mutex> lock(runtime->mutex);
    if (runtime->command_id != command_id ||
        runtime->generation != generation) {
      return false;
    }
    old_handle = runtime->lifecycle_handle;
    runtime->lifecycle_handle.reset();
    schedule_generation = ++runtime->lifecycle_schedule_generation;
  }
  if (old_handle.has_value()) {
    try {
      scheduler_.cancel(*old_handle);
    } catch (...) {
      // The schedule generation already invalidated the old callback.
    }
  }

  core::ScheduleHandle handle;
  try {
    handle = scheduler_.schedule(
        deadline_ns,
        [this, runtime, command_id, generation, schedule_generation]() {
          advance_gripper_command(runtime, command_id, generation,
                                  schedule_generation);
        },
        core::SchedulerPriority::ControllerCommand);
  } catch (...) {
    std::lock_guard<std::mutex> lock(runtime->mutex);
    if (runtime->command_id == command_id &&
        runtime->generation == generation &&
        runtime->lifecycle_schedule_generation == schedule_generation) {
      runtime->state = "failed";
      runtime->executing = false;
      runtime->goal_accepted = false;
      runtime->cancel_pending = false;
      runtime->projection_state = core::GripperProjectionState::kNotRequired;
      runtime->projection_reason.clear();
      runtime->error = "Gripper lifecycle could not be scheduled";
      runtime->reason_code = ros2_control_fault_reason::kGripperRequestFailed;
      ++runtime->revision;
    }
    return false;
  }
  bool retained = false;
  {
    std::lock_guard<std::mutex> lock(runtime->mutex);
    const bool lifecycle_active =
        runtime->executing ||
        (runtime->state == "completed" &&
         runtime->projection_state == core::GripperProjectionState::kPending);
    if (runtime->command_id == command_id &&
        runtime->generation == generation &&
        runtime->lifecycle_schedule_generation == schedule_generation &&
        lifecycle_active) {
      runtime->lifecycle_handle = handle;
      retained = true;
    }
  }
  if (!retained) {
    try {
      scheduler_.cancel(handle);
    } catch (...) {
      // A stale queued callback is generation-fenced at entry.
    }
  }
  return retained;
}

void Ros2ControlExecutionBackend::advance_gripper_command(
    const std::shared_ptr<GripperRuntime> &runtime, std::uint64_t command_id,
    std::uint64_t generation, std::uint64_t schedule_generation) {
  GoalHandleGripper::SharedPtr goal_handle;
  rclcpp_action::Client<GripperCommand>::SharedPtr action_client;
  Ros2GripperActionConfig runtime_config;
  std::optional<double> scene_position;
  bool schedule_next = false;
  core::TimePoint next_deadline_ns = 0;
  {
    std::lock_guard<std::mutex> lock(runtime->mutex);
    if (runtime->command_id != command_id ||
        runtime->generation != generation ||
        runtime->lifecycle_schedule_generation != schedule_generation) {
      return;
    }
    runtime->lifecycle_handle.reset();
    runtime_config = runtime->config;
    const auto now_ns = clock_.now();
    const auto duration_ns =
        std::max<core::Duration>(1, gripper_duration_ns(runtime->duration_s));
    const auto elapsed_ns =
        std::max<core::Duration>(0, now_ns - runtime->started_at_ns);
    runtime->progress = runtime->duration_s <= 0.0
                            ? 1.0
                            : clamp_progress(static_cast<double>(elapsed_ns) /
                                             static_cast<double>(duration_ns));

    if (runtime->executing && runtime->deadline_at_ns.has_value() &&
        now_ns >= *runtime->deadline_at_ns) {
      goal_handle = runtime->active_goal_handle;
      action_client = runtime->action_client;
      runtime->active_goal_handle.reset();
      runtime->state = "failed";
      runtime->executing = false;
      runtime->goal_accepted = false;
      runtime->cancel_pending = false;
      runtime->error = "Timed out waiting for gripper action result";
      runtime->reason_code = ros2_control_fault_reason::kGripperResultTimeout;
    } else if (runtime->executing &&
               runtime_config.transport_kind == "isaac_joint_state_effort" &&
               (runtime->duration_s <= 0.0 || elapsed_ns >= duration_ns)) {
      runtime->state = "completed";
      runtime->executing = false;
      runtime->goal_accepted = false;
      runtime->cancel_pending = false;
      runtime->progress = 1.0;
      runtime->error.clear();
      runtime->reason_code.clear();
      runtime->current_position = runtime->target_position;
      scene_position = runtime->target_position;
    } else if (!runtime_config.group_id.empty() &&
               !runtime_config.joint_name.empty()) {
      if (runtime->executing && is_commanded_sim_mode() &&
          runtime->start_position.has_value()) {
        scene_position = *runtime->start_position +
                         (runtime->target_position - *runtime->start_position) *
                             runtime->progress;
        runtime->current_position = scene_position;
      } else if (!runtime->executing &&
                 runtime->projection_state ==
                     core::GripperProjectionState::kPending) {
        // ROS action feedback/result callbacks own live observed projection.
        // The lifecycle timer only interpolates commanded-sim state or retries
        // a terminal projection; replaying an unchanged feedback sample at
        // 100 Hz would create avoidable scene commits and contention.
        if (runtime->current_position.has_value()) {
          scene_position = *runtime->current_position;
        } else if (runtime->start_position.has_value()) {
          scene_position = *runtime->start_position;
        }
      }
    }
    ++runtime->revision;
    schedule_next = runtime->executing;
    next_deadline_ns = now_ns + 10'000'000;
    if (runtime->executing &&
        runtime_config.transport_kind == "isaac_joint_state_effort") {
      next_deadline_ns =
          std::min(next_deadline_ns,
                   gripper_deadline(runtime->started_at_ns, duration_ns));
    } else if (runtime->deadline_at_ns.has_value()) {
      next_deadline_ns = std::min(next_deadline_ns, *runtime->deadline_at_ns);
    }
  }

  if (scene_position.has_value()) {
    std::lock_guard<std::mutex> lock(runtime->mutex);
    if (runtime->command_id == command_id &&
        runtime->generation == generation &&
        runtime->lifecycle_schedule_generation == schedule_generation) {
      const bool terminal = !runtime->executing;
      const bool queued = schedule_gripper_scene_projection_locked(
          runtime, *scene_position, terminal);
      if (!queued && terminal) {
        ++runtime->projection_retry_count;
        if (runtime->projection_retry_count >= 10) {
          runtime->projection_state = core::GripperProjectionState::kFailed;
          runtime->projection_reason = "scene_sink_retry_exhausted";
          runtime->state = "failed";
          runtime->error =
              "Timed out queueing the terminal gripper scene position";
          runtime->reason_code = "gripper_projection_failed";
        } else {
          schedule_next = true;
          next_deadline_ns = clock_.now() + 20'000'000;
        }
      }
      ++runtime->revision;
    }
  }
  if (action_client && goal_handle) {
    try {
      (void)action_client->async_cancel_goal(goal_handle);
    } catch (const std::exception &exc) {
      RCLCPP_WARN(node_->get_logger(),
                  "Failed to cancel timed-out gripper action goal: %s",
                  exc.what());
    }
  }
  if (schedule_next) {
    (void)schedule_gripper_lifecycle(runtime, command_id, generation,
                                     next_deadline_ns);
  }
}

core::GripperCommandStatusDTO Ros2ControlExecutionBackend::get_gripper_status(
    const std::string &gripper_id) const {
  std::shared_ptr<GripperRuntime> runtime;
  {
    std::lock_guard<std::mutex> lock(gripper_map_mutex_);
    const auto it = grippers_.find(gripper_id);
    if (it == grippers_.end()) {
      core::GripperCommandStatusDTO status;
      status.gripper_id = gripper_id;
      return status;
    }
    runtime = it->second;
  }
  std::lock_guard<std::mutex> lock(runtime->mutex);
  core::GripperCommandStatusDTO status;
  status.state =
      runtime->projection_state == core::GripperProjectionState::kFailed
          ? core::GripperCommandState::kFailed
      : runtime->state == "completed" &&
              runtime->projection_state ==
                  core::GripperProjectionState::kPending
          ? core::GripperCommandState::kExecuting
      : runtime->state == "executing" ? core::GripperCommandState::kExecuting
      : runtime->state == "completed" ? core::GripperCommandState::kCompleted
      : runtime->state == "failed"    ? core::GripperCommandState::kFailed
                                      : core::GripperCommandState::kIdle;
  status.gripper_id = gripper_id;
  status.command_id = runtime->command_id;
  status.generation = runtime->generation;
  status.revision = runtime->revision;
  status.progress = runtime->progress;
  status.current_position = runtime->current_position;
  // Return the last owner-published observation verbatim. Freshness is an
  // admission/evidence concern at the owning callback boundary; deriving it
  // from wall time here would make this supposedly immutable status change
  // without a publication or revision increment.
  status.observed_position = runtime->observed_position;
  status.target_position = runtime->target_position;
  if (runtime->command_id != 0) {
    status.commanded_position = runtime->target_position;
  }
  status.commanded_state = runtime->commanded_state;
  status.actuation_kind = runtime->actuation_kind;
  status.state_authority = runtime->state_authority;
  status.completion_policy = runtime->completion_policy;
  status.grasp_confirmation_policy = runtime->grasp_confirmation_policy;
  status.transport_kind = runtime->transport_kind;
  status.evidence = runtime->evidence;
  status.evidence_state = runtime->evidence_state;
  status.projection_state = runtime->projection_state;
  status.projection_reason = runtime->projection_reason;
  if (!runtime->error.empty() || !runtime->reason_code.empty()) {
    status.error =
        core::ErrorDTO{runtime->reason_code.empty() ? "gripper_request_failed"
                                                    : runtime->reason_code,
                       runtime->error,
                       {}};
  }
  return status;
}

// =================================================================
// FollowJointTrajectory preempt/controlled-stop dispatch helpers.
//
// Anchored two-point FJT goals preserve JTC spline continuity across
// preempts. Used by the motion-interrupt controlled-stop paths
// (spring_back_hold / soft_on_path) — see execute_controlled_stop.
// =================================================================

bool Ros2ControlExecutionBackend::send_fjt_replacement_trajectory(
    const core::TrajectoryDTO &trajectory, bool track_terminal_goal) {
  const auto callback_execution_generation =
      running_generation_.load(std::memory_order_acquire);
  std::uint64_t managed_generation = 0;
  if (track_terminal_goal) {
    std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
    managed_generation = ++managed_fjt_generation_;
    forced_managed_stop_execution_generation_.reset();
    forced_managed_stop_generation_.reset();
    managed_fjt_terminal_goal_id_.reset();
    managed_fjt_terminal_result_received_ = false;
    managed_fjt_terminal_result_success_ = false;
    managed_fjt_terminal_result_received_at_.reset();
    managed_fjt_terminal_error_.reset();
  }
  const auto fail_terminal = [&](std::string message,
                                 std::string failure_stage) {
    if (!track_terminal_goal) {
      return;
    }
    std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
    if (managed_generation != managed_fjt_generation_) {
      return;
    }
    managed_fjt_terminal_result_received_ = true;
    managed_fjt_terminal_result_success_ = false;
    managed_fjt_terminal_error_ =
        make_error(core::ErrorCode::kStopFailed, std::move(message),
                   {{"reason_code", "soft_on_path_dispatch_failed"},
                    {"failure_stage", std::move(failure_stage)}});
    managed_fjt_cv_.notify_all();
  };

  if (trajectory.joint_names.empty() || trajectory.waypoints.empty()) {
    fail_terminal("soft_on_path replacement trajectory is empty",
                  "empty_replacement_trajectory");
    return false;
  }
  double previous_time = -std::numeric_limits<double>::infinity();
  for (std::size_t i = 0; i < trajectory.waypoints.size(); ++i) {
    const auto &waypoint = trajectory.waypoints[i];
    if (waypoint.positions_rad.size() != trajectory.joint_names.size()) {
      fail_terminal("soft_on_path replacement waypoint position size does not "
                    "match joints",
                    "replacement_position_size_mismatch");
      return false;
    }
    if (!waypoint.velocities_rad_s.empty() &&
        waypoint.velocities_rad_s.size() != trajectory.joint_names.size()) {
      fail_terminal("soft_on_path replacement waypoint velocity size does not "
                    "match joints",
                    "replacement_velocity_size_mismatch");
      return false;
    }
    if (!waypoint.accelerations_rad_s2.empty() &&
        waypoint.accelerations_rad_s2.size() != trajectory.joint_names.size()) {
      fail_terminal("soft_on_path replacement waypoint acceleration size does "
                    "not match joints",
                    "replacement_acceleration_size_mismatch");
      return false;
    }
    // Allow a single leading negative-time continuity anchor (i == 0): the
    // dispatch-latency shift (shift_soft_stop_for_dispatch_latency) and the
    // seam-preserving preempt prepend one past-anchor carrying the live seam
    // state so stock JTC's spline starts continuously (no backward-pull bump),
    // without using a cancel-first hold. Every later point must be
    // non-negative; all points must be finite and monotonically increasing.
    const bool leading_anchor = (i == 0);
    if (!std::isfinite(waypoint.time_from_start_s) ||
        (!leading_anchor && waypoint.time_from_start_s < 0.0) ||
        waypoint.time_from_start_s <= previous_time) {
      fail_terminal("soft_on_path replacement waypoint times must be finite "
                    "and monotonic",
                    "replacement_time_invalid");
      return false;
    }
    previous_time = waypoint.time_from_start_s;
  }

  if (!wait_for_action_server_until(action_client_,
                                    config_.wait_for_action_server_timeout_s)) {
    fail_terminal("FollowJointTrajectory action server unavailable for "
                  "soft_on_path path ramp",
                  "action_server_unavailable");
    return false;
  }

  const auto mark_terminal_dispatch_failed = [&](std::string message) {
    if (!track_terminal_goal) {
      return;
    }
    std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
    if (managed_generation != managed_fjt_generation_) {
      return;
    }
    managed_fjt_terminal_result_received_ = true;
    managed_fjt_terminal_result_success_ = false;
    managed_fjt_terminal_error_ =
        make_error(core::ErrorCode::kStopFailed, std::move(message));
    managed_fjt_cv_.notify_all();
  };

  FollowJointTrajectory::Goal goal;
  goal.trajectory.joint_names = transport_joint_names();
  if (config_.disable_controller_path_tolerances) {
    disable_fjt_controller_tolerances(goal, transport_joint_names());
  }
  for (const auto &waypoint : trajectory.waypoints) {
    trajectory_msgs::msg::JointTrajectoryPoint point;
    point.positions = waypoint.positions_rad;
    point.velocities = waypoint.velocities_rad_s;
    point.accelerations = waypoint.accelerations_rad_s2;
    point.time_from_start = duration_from_seconds(waypoint.time_from_start_s);
    goal.trajectory.points.push_back(std::move(point));
  }

  rclcpp_action::Client<FollowJointTrajectory>::SendGoalOptions options;
  options.feedback_callback =
      [this](GoalHandleFJT::SharedPtr goal_handle,
             const std::shared_ptr<const FollowJointTrajectory::Feedback>
                 feedback) { on_action_feedback(goal_handle, feedback); };
  options.result_callback = [this, track_terminal_goal, managed_generation,
                             callback_execution_generation](
                                const GoalHandleFJT::WrappedResult &result) {
    if (track_terminal_goal) {
      on_managed_fjt_terminal_result(result, managed_generation);
    } else {
      on_action_result(result, callback_execution_generation);
    }
  };

  auto goal_handle_future = action_client_->async_send_goal(goal, options);
  if (goal_handle_future.wait_for(std::chrono::duration<double>(
          config_.wait_for_action_server_timeout_s)) !=
      std::future_status::ready) {
    cancel_all_fjt_goals();
    mark_terminal_dispatch_failed(
        "timed out waiting for soft_on_path path-ramp goal acceptance");
    return false;
  }
  GoalHandleFJT::SharedPtr goal_handle;
  try {
    goal_handle = goal_handle_future.get();
  } catch (const std::exception &) {
    mark_terminal_dispatch_failed(
        "soft_on_path path-ramp goal acceptance threw an exception");
    return false;
  }
  if (!goal_handle) {
    mark_terminal_dispatch_failed(
        "soft_on_path path-ramp goal was rejected by the controller");
    return false;
  }
  bool stale_terminal_generation = false;
  {
    std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
    if (track_terminal_goal && managed_generation != managed_fjt_generation_) {
      stale_terminal_generation = true;
    } else {
      managed_fjt_goal_ = goal_handle;
      if (track_terminal_goal) {
        managed_fjt_terminal_goal_id_ = goal_handle->get_goal_id();
        managed_fjt_goal_accepted_at_ = std::chrono::steady_clock::now();
      }
    }
  }
  if (stale_terminal_generation) {
    cancel_goal_if_known(goal_handle);
    return false;
  }
  return true;
}

void Ros2ControlExecutionBackend::begin_controlled_stop_path_deviation_tracking(
    std::shared_ptr<const core::TrajectoryDTO> accepted_trajectory,
    double initial_path_deviation_rad) {
  std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
  controlled_stop_path_deviation_reference_ = std::move(accepted_trajectory);
  controlled_stop_path_deviation_max_rad_ =
      std::isfinite(initial_path_deviation_rad)
          ? std::max(0.0, initial_path_deviation_rad)
          : 0.0;
  controlled_stop_path_deviation_feedback_samples_ = 0;
  controlled_stop_path_deviation_error_.reset();
  controlled_stop_path_deviation_tracking_active_.store(
      true, std::memory_order_release);
}

void Ros2ControlExecutionBackend::
    record_controlled_stop_path_deviation_feedback(
        const std::vector<std::string> &joint_names,
        const std::vector<double> &positions) {
  if (!controlled_stop_path_deviation_tracking_active_.load(
          std::memory_order_acquire)) {
    return;
  }
  std::shared_ptr<const core::TrajectoryDTO> reference;
  {
    std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
    reference = controlled_stop_path_deviation_reference_;
    if (!reference) {
      return;
    }
  }
  if (joint_names.empty() || positions.empty()) {
    std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
    if (controlled_stop_path_deviation_reference_ != reference) {
      return;
    }
    controlled_stop_path_deviation_error_ = make_error(
        core::ErrorCode::kMotionInterruptStopStateInvalid,
        "soft_on_path path-ramp feedback omitted joint names or positions",
        {{"reason", "path_deviation_feedback_empty"}});
    return;
  }
  core::RobotStateDTO observed;
  observed.joint_names = joint_names;
  observed.positions_rad = positions;
  auto measured =
      core::measure_soft_on_path_path_deviation(*reference, observed);
  std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
  if (controlled_stop_path_deviation_reference_ != reference) {
    return;
  }
  if (!measured.ok()) {
    controlled_stop_path_deviation_error_ = measured.error();
    return;
  }
  controlled_stop_path_deviation_max_rad_ = std::max(
      controlled_stop_path_deviation_max_rad_.value_or(0.0), measured.value());
  ++controlled_stop_path_deviation_feedback_samples_;
}

void Ros2ControlExecutionBackend::
    clear_controlled_stop_path_deviation_tracking() {
  controlled_stop_path_deviation_tracking_active_.store(
      false, std::memory_order_release);
  std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
  controlled_stop_path_deviation_reference_.reset();
  controlled_stop_path_deviation_max_rad_.reset();
  controlled_stop_path_deviation_feedback_samples_ = 0;
  controlled_stop_path_deviation_error_.reset();
}

void Ros2ControlExecutionBackend::on_managed_fjt_terminal_result(
    const GoalHandleFJT::WrappedResult &result, std::uint64_t generation) {
  const auto now = std::chrono::steady_clock::now();
  action_result_callbacks_total_.fetch_add(1, std::memory_order_relaxed);
  const bool controller_success =
      result.result &&
      result.result->error_code == FollowJointTrajectory::Result::SUCCESSFUL;
  const bool terminal_success =
      result.code == rclcpp_action::ResultCode::SUCCEEDED && controller_success;

  std::optional<std::uint64_t> forced_execution_generation;
  {
    std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
    if (generation != managed_fjt_generation_ ||
        !managed_fjt_terminal_goal_id_.has_value() ||
        result.goal_id != *managed_fjt_terminal_goal_id_) {
      return;
    }
    record_action_result_latency_ns(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            now - managed_fjt_goal_accepted_at_)
            .count());
    managed_fjt_terminal_result_received_ = true;
    managed_fjt_terminal_result_success_ = terminal_success;
    managed_fjt_terminal_result_received_at_ = now;
    managed_fjt_goal_.reset();
    managed_fjt_terminal_goal_id_.reset();
    if (!terminal_success) {
      const std::string controller_error =
          result.result ? result.result->error_string
                        : "controller returned no result";
      managed_fjt_terminal_error_ = make_error(
          core::ErrorCode::kStopFailed,
          controller_error.empty()
              ? "managed FJT terminal soft-stop goal failed"
              : "managed FJT terminal soft-stop goal failed: " +
                    controller_error,
          {{"result_code", std::to_string(static_cast<int>(result.code))}});
    } else {
      managed_fjt_terminal_error_.reset();
    }
    if (forced_managed_stop_generation_ == generation &&
        forced_managed_stop_execution_generation_.has_value()) {
      forced_execution_generation =
          forced_managed_stop_execution_generation_;
    }
    forced_managed_stop_execution_generation_.reset();
    forced_managed_stop_generation_.reset();
    managed_fjt_cv_.notify_all();
  }
  if (forced_execution_generation.has_value()) {
    (void)complete_forced_managed_stop_execution(
        *forced_execution_generation);
  }
}

void Ros2ControlExecutionBackend::set_runtime_velocity_scale(double scale) {
  // ADR-0018: clamp + atomic store. Caller (kernel) already validated
  // finiteness, but defense-in-depth keeps monitor/dispatcher workers from
  // ever reading NaN/Inf via this path.
  if (!std::isfinite(scale))
    return;
  if (scale < 0.0)
    scale = 0.0;
  if (scale > 1.0)
    scale = 1.0;
  {
    std::lock_guard<std::mutex> lock(runtime_velocity_scale_mutex_);
    if (controlled_stop_runtime_scale_freeze_depth_ > 0) {
      // M4: the owned speed-scaling publisher is frozen while a controlled
      // stop is in flight. Queue operator/API intent and publish it only after
      // the stop window ends; never retime the active stop trajectory.
      deferred_runtime_velocity_scale_ = scale;
      return;
    }
    runtime_velocity_scale_.store(scale, std::memory_order_release);
  }
  publish_runtime_velocity_scale(scale);
  execution_cv_.notify_all();
  managed_fjt_cv_.notify_all();
}

} // namespace manyforge::ros2_control_execution
