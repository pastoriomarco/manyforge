#pragma once

#include "../../manyforge_ros2_control_execution/src/ros2_control_stop_qualification_probe.hpp"

#include <control_msgs/action/follow_joint_trajectory.hpp>
#include <manyforge_core/ports/iexecution_state_sink.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/types/motion_interrupt.hpp>
#include <manyforge_core/types/participant_stop_stage.hpp>
#include <manyforge_core/types/trajectory_dto.hpp>
#include <rclcpp/rclcpp.hpp>
#include <rclcpp_action/rclcpp_action.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace manyforge::ros2_control_execution::qualification_fixture {

using FollowJointTrajectory = control_msgs::action::FollowJointTrajectory;
using ServerGoalHandleFJT =
    rclcpp_action::ServerGoalHandle<FollowJointTrajectory>;

[[nodiscard]] inline std::string unique_suffix() {
  static std::atomic<std::uint64_t> next{1U};
  return std::to_string(next.fetch_add(1U, std::memory_order_relaxed));
}

class StateSink final : public core::IExecutionStateSink {
public:
  core::ExecutionStateSinkResult apply_group_joint_positions(
      const std::string &group_id, const std::vector<double> &positions_rad,
      core::ExecutionStateUpdateMode) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (group_id != "arm" || positions_rad.size() != 1U) {
      return {core::ExecutionStateSinkStatus::kInvalidPositions};
    }
    position_ = positions_rad.front();
    return {};
  }

  [[nodiscard]] std::optional<double>
  group_joint_position(const std::string &group_id,
                       const std::string &joint_name) const override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (group_id != "arm" || joint_name != "joint_1") {
      return std::nullopt;
    }
    return position_;
  }

  core::ExecutionStateSinkResult apply_group_joint_position(
      const std::string &group_id, const std::string &joint_name,
      double position_rad, core::ExecutionStateUpdateMode) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (group_id != "arm" || joint_name != "joint_1") {
      return {core::ExecutionStateSinkStatus::kInvalidJoint};
    }
    position_ = position_rad;
    return {};
  }

private:
  mutable std::mutex mutex_;
  double position_{};
};

class FakeFjtServer final {
public:
  explicit FakeFjtServer(const std::string &action_name) {
    context_ = std::make_shared<rclcpp::Context>();
    int argc = 0;
    char **argv = nullptr;
    context_->init(argc, argv);
    rclcpp::NodeOptions options;
    options.context(context_);
    node_ = std::make_shared<rclcpp::Node>(
        "manyforge_ros_stop_qualification_server_" + unique_suffix(), options);
    server_ = rclcpp_action::create_server<FollowJointTrajectory>(
        node_, action_name,
        [](const rclcpp_action::GoalUUID &,
           std::shared_ptr<const FollowJointTrajectory::Goal>) {
          return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE;
        },
        [this](const std::shared_ptr<ServerGoalHandleFJT>) {
          cancel_requests_.fetch_add(1U, std::memory_order_relaxed);
          cv_.notify_all();
          return rclcpp_action::CancelResponse::ACCEPT;
        },
        [this](const std::shared_ptr<ServerGoalHandleFJT> goal_handle) {
          std::shared_ptr<ServerGoalHandleFJT> previous;
          {
            std::lock_guard<std::mutex> lock(mutex_);
            previous = current_goal_;
            current_goal_ = goal_handle;
            ++goal_count_;
          }
          cv_.notify_all();
          if (previous && previous->is_active()) {
            auto result = std::make_shared<FollowJointTrajectory::Result>();
            result->error_code = FollowJointTrajectory::Result::SUCCESSFUL;
            previous->abort(result);
          }
        });
    rclcpp::ExecutorOptions executor_options;
    executor_options.context = context_;
    executor_ = std::make_shared<rclcpp::executors::SingleThreadedExecutor>(
        executor_options);
    executor_->add_node(node_);
    spin_thread_ = std::thread([this]() { executor_->spin(); });
  }

  FakeFjtServer(const FakeFjtServer &) = delete;
  FakeFjtServer &operator=(const FakeFjtServer &) = delete;

  ~FakeFjtServer() {
    abort_current_goal();
    server_.reset();
    if (executor_) {
      executor_->cancel();
    }
    if (context_) {
      context_->shutdown("ROS Stop qualification server shutdown");
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

  [[nodiscard]] bool wait_for_goal_count(
      std::uint64_t expected,
      std::chrono::milliseconds timeout = std::chrono::seconds(2)) const {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout,
                        [this, expected]() { return goal_count_ >= expected; });
  }

  [[nodiscard]] bool wait_for_cancel_requests(
      std::uint64_t expected,
      std::chrono::milliseconds timeout = std::chrono::seconds(2)) const {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout, [this, expected]() {
      return cancel_requests_.load(std::memory_order_relaxed) >= expected;
    });
  }

  [[nodiscard]] std::uint64_t goal_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return goal_count_;
  }

  [[nodiscard]] std::uint64_t cancel_requests() const {
    return cancel_requests_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] bool publish_feedback(double actual_position = 0.0) {
    std::shared_ptr<ServerGoalHandleFJT> goal;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      goal = current_goal_;
    }
    if (!goal || !goal->is_active()) {
      return false;
    }
    auto feedback = std::make_shared<FollowJointTrajectory::Feedback>();
    feedback->joint_names = {"source_joint_1"};
    feedback->desired.positions = {actual_position};
    feedback->desired.velocities = {0.0};
    feedback->actual.positions = {actual_position};
    feedback->actual.velocities = {0.0};
    feedback->error.positions = {0.0};
    feedback->error.velocities = {0.0};
    goal->publish_feedback(feedback);
    return true;
  }

  [[nodiscard]] bool cancel_current_goal() {
    std::shared_ptr<ServerGoalHandleFJT> goal;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      goal = current_goal_;
    }
    if (!goal || !goal->is_canceling()) {
      return false;
    }
    auto result = std::make_shared<FollowJointTrajectory::Result>();
    result->error_code = FollowJointTrajectory::Result::SUCCESSFUL;
    goal->canceled(result);
    return true;
  }

private:
  void abort_current_goal() noexcept {
    std::shared_ptr<ServerGoalHandleFJT> goal;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      goal = current_goal_;
    }
    if (!goal || !goal->is_active()) {
      return;
    }
    try {
      auto result = std::make_shared<FollowJointTrajectory::Result>();
      goal->abort(result);
    } catch (...) {
    }
  }

  std::shared_ptr<rclcpp::Context> context_;
  rclcpp::Node::SharedPtr node_;
  rclcpp_action::Server<FollowJointTrajectory>::SharedPtr server_;
  std::shared_ptr<rclcpp::executors::SingleThreadedExecutor> executor_;
  std::thread spin_thread_;
  mutable std::mutex mutex_;
  mutable std::condition_variable cv_;
  std::shared_ptr<ServerGoalHandleFJT> current_goal_;
  std::uint64_t goal_count_{};
  std::atomic<std::uint64_t> cancel_requests_{0U};
};

[[nodiscard]] inline core::RuntimeParticipantBinding make_binding() {
  core::RuntimeParticipantBindingDTO value{
      core::ProgramId("program.ros2_control.stop_qualification"),
      core::ProgramRevision("program-revision"),
      core::ProgramBindingId("program-binding"),
      core::RetainedBundleId("bundle"),
      core::GraphRevision("graph"),
      core::ComponentRoleId("robot"),
      core::TargetRoleId("arm"),
      core::ControllableComponentId("robot.1"),
      core::ComponentTypeId("robot.type"),
      core::ComponentTypeRevision("robot-type-revision"),
      core::CommandTargetId("arm.target"),
      core::CapabilityId("execution.trajectory.execute"),
      core::CapabilityDefinitionRevision("capability-revision"),
      std::nullopt,
      {core::ProgramModelRoleBindingDTO{core::ModelRoleId("body"),
                                        core::ModelInstanceId("robot.model"),
                                        core::ModelRevision("model-revision")}},
      {core::RuntimeModelGroupBindingDTO{core::ModelGroupRoleId("arm_group"),
                                         core::ModelInstanceId("robot.model"),
                                         "arm", {"joint_1"}}},
      {core::ProgramFrameRoleBindingDTO{
          core::FrameRoleId("tcp"), core::ModelInstanceId("robot.model"),
          core::ModelRevision("model-revision"), "tcp"}},
      {core::RuntimeResourceBindingDTO{
          core::ResourceRoleId("joint_1"), core::ResourceId("robot.joint.1"),
          "source_joint_1", "joint_1", core::CoordinateKind::kAngular}},
      {},
      core::RuntimeControlPathDTO{
          core::ControlDomainId("domain.arm"),
          core::ControlDomainRevision("domain-revision"),
          core::ControlBindingId("binding.arm"),
          core::ControlBindingRevision("binding-revision"),
          core::ControlInterfaceContractId("interface.trajectory"),
          core::ControlInterfaceRevision("interface-revision"),
          core::ControllerTopologyId("topology.ros2_control"),
          core::TopologyRevision("topology-revision"),
          core::ControlRouteId("route.ros2_control"),
          core::ActivationSetId("activation.ros2_control"),
          core::ActiveControlConfigurationId("active.ros2_control"),
          core::ActiveControlConfigurationGeneration(1U),
          core::ProviderInstanceId("provider.ros2_control"),
          core::ProviderManifestId("manyforge.provider.ros2_control"),
          core::ProviderManifestRevision("provider-manifest-revision"),
          core::RuntimeUnitId("runtime.ros2_control"),
          core::RuntimeEpoch("runtime-epoch.1"),
          core::StateAuthorityId("state.commanded"),
          core::StateAuthorityClass::kCommanded,
          core::StopEvidencePolicyId("stop.ros2_control"),
          core::StopEvidencePolicyRevision("stop-revision"),
          core::ContainmentGranularity::kResource,
          std::nullopt},
      std::nullopt,
      {},
      {core::RuntimeUnitEpochBindingDTO{
          core::RuntimeUnitId("runtime.ros2_control"),
          core::RuntimeEpoch("runtime-epoch.1")}}};
  auto sealed = core::verify_runtime_participant_binding(std::move(value));
  if (!sealed.ok()) {
    throw std::logic_error(sealed.error().message);
  }
  return std::move(sealed.value());
}

[[nodiscard]] inline core::ResolvedParticipantStopStageDTO
software_hard_stage(const core::RuntimeParticipantBinding &binding,
                    core::ProductContractRevision product_revision) {
  const core::OperationId operation_id("operation.ros_stop_qualification");
  const core::OperationRevision operation_revision(1U);
  const core::OperationParticipantId participant_id(
      "participant.ros_stop_qualification");
  const auto &value = binding.value();
  const auto &path = *value.control_path;
  const std::vector<core::ResourceId> resources{
      value.resources.front().resource_id};
  core::ResolvedStopRuntimeTargetDTO target{
      path.provider_instance_id,
      path.provider_manifest_id,
      path.provider_manifest_revision,
      path.provider_runtime_unit_id,
      core::ProductId("product.ros2_control.stop_qualification"),
      core::ProductContractId(
          "manyforge.ros2_control_execution_runtime_unit"),
      std::move(product_revision),
      core::ContentDigest("runtime-target-digest"),
      path.provider_runtime_epoch,
      path.provider_runtime_epoch,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt};
  core::ResolvedStopEscalationStageDTO resolved_command{
      0U,
      core::StopEscalationStageId("stage.software_hard"),
      core::StopEscalationStageKind::kSoftwareHard,
      core::ContainmentGranularity::kResource,
      core::StopContainmentCommandDTO{
          core::StopBudgetPhaseId("hard_command"),
          core::StopContainmentCommandKind::kRegistered,
          std::nullopt,
          core::RegisteredStopCommandKindId("manyforge.stop.software_hard")},
      std::nullopt,
      core::ResolvedStopCommandQualificationDTO{},
      target,
      core::ResolvedStopResourceContainmentScopeDTO{path.activation_set_id,
                                                    resources},
      core::StopEvidenceConjunction::kAllOf,
      {core::ResolvedStopEvidenceRequirementDTO{
          core::StopEvidenceRequirementDTO{
              core::StopEvidenceItemId("hard.rest"),
              core::StopBudgetPhaseId("hard_terminal"),
              core::StopEvidenceRequirementKind::kMotionRest,
              std::nullopt,
              std::nullopt,
              core::StopEvidenceSourceRequirementDTO{
                  core::StopEvidenceSourceKind::kBindingStateAuthority,
                  100'000'000,
                  std::nullopt,
                  {core::StateAuthorityClass::kCommanded}},
              core::StopEvidenceObservationDTO{
                  core::StopEvidenceObservationKind::kPostCommandDwell,
                  10'000'000}},
          core::ResolvedStopEvidenceSourceDTO{
              core::StopEvidenceSourceKind::kBindingStateAuthority,
              target,
              path.state_authority_id,
              path.state_authority_class,
              std::nullopt}}}};
  return core::ResolvedParticipantStopStageDTO{
      core::StopId("stop.ros_stop_qualification"),
      core::StopGeneration(1U),
      operation_id,
      operation_revision,
      participant_id,
      core::ControllerGoalId("goal.ros_stop_qualification"),
      core::ControllerGoalGeneration(1U),
      value.controllable_component_id,
      value.command_target_id,
      value.capability_id,
      path.control_domain_id,
      path.control_domain_revision,
      path.control_binding_id,
      path.control_binding_revision,
      path.active_control_configuration_id,
      path.active_control_configuration_generation,
      path.controller_topology_id,
      path.topology_revision,
      path.control_route_id,
      std::nullopt,
      std::nullopt,
      path.provider_runtime_epoch,
      resources,
      core::ContainmentGranularity::kResource,
      path.stop_evidence_policy_id,
      path.stop_evidence_policy_revision,
      core::PreparedParticipantStopPlanDTO{
          core::ContentDigest("plan.digest"),
          core::ResolvedStopBasePlanId("base.plan"),
          operation_id,
          operation_revision,
          participant_id,
          value.program_id,
          value.program_revision,
          value.program_binding_id,
          core::ProgramRunId("program.run"),
          std::nullopt,
          core::ParticipantStopProfilePurposeDTO::kOrdinary,
          std::nullopt,
          core::StopDeadlineBudgetDTO{
              2'000'000'000,
              100'000'000,
              {{core::StopBudgetPhaseId("hard_command"), 500'000'000},
               {core::StopBudgetPhaseId("hard_terminal"), 1'900'000'000}}},
          {core::PreparedStopStageCommandDTO{std::move(resolved_command),
                                             std::nullopt, std::nullopt}}},
      0U,
      core::ContentDigest("scope.digest")};
}

[[nodiscard]] inline core::RemainingStopBudgetDTO
remaining_budget(const core::ResolvedParticipantStopStageDTO &stage) {
  core::RemainingStopBudgetDTO budget{
      stage.stop_id, stage.stop_generation,
      stage.participant_stop_plan.deadline_budget.outer_duration_ns, {}};
  for (const auto &phase :
       stage.participant_stop_plan.deadline_budget.phases) {
    budget.phases.push_back({phase.phase_id, phase.deadline_offset_ns});
  }
  return budget;
}

[[nodiscard]] inline core::TrajectoryDTO trajectory() {
  core::TrajectoryDTO result;
  result.joint_names = {"joint_1"};
  result.waypoints = {
      core::TrajectoryWaypointDTO{0.0, {0.0}, {0.0}, {0.0}},
      core::TrajectoryWaypointDTO{30.0, {0.5}, {0.0}, {0.0}}};
  return result;
}

[[nodiscard]] inline core::MotionInterruptStopProfileDTO spring_profile() {
  core::MotionInterruptStopProfileDTO profile;
  profile.profile_id = "ros2_control_stop_qualification_spring";
  profile.stop_mode = core::MotionInterruptStopMode::kSpringBackHold;
  profile.deceleration_time_s = 1.0;
  profile.require_backend_confirmation = true;
  profile.per_joint_max_deceleration = {
      {"joint_1", core::AngularAccelerationDTO{100.0}}};
  return profile;
}

[[nodiscard]] inline Ros2ControlExecutionBackendConfig
backend_config(const core::RuntimeParticipantBinding &binding,
               const std::string &action_name) {
  Ros2ControlExecutionBackendConfig config;
  config.node_name = "manyforge_ros_stop_qualification_backend_" +
                     unique_suffix();
  config.action_name = action_name;
  config.joint_state_topic =
      "/manyforge_ros_stop_qualification_joint_states_" + unique_suffix();
  config.controller_state_topic.clear();
  config.required_joints = {"joint_1"};
  config.transport_joint_names = {"source_joint_1"};
  config.execution_state_source_mode = "commanded_sim";
  config.initial_joint_positions_rad = {0.0};
  config.prepared_participant_binding = binding;
  config.wait_for_state_timeout_s = 2.0;
  config.wait_for_action_server_timeout_s = 2.0;
  config.stop_completion_timeout_s = 1.0;
  config.joint_state_max_age_s = 5.0;
  config.monitor_period_s = 0.01;
  config.soft_on_path_stop_traj_delta_s = 0.1;
  config.soft_on_path_sample_period_s = 0.01;
  config.soft_on_path_max_jerk_rad_s3 = {{"joint_1", 1'000'000.0}};
  config.enable_jtc_speed_scaling_input = false;
  return config;
}

} // namespace manyforge::ros2_control_execution::qualification_fixture
