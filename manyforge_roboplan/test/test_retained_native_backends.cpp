#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <limits>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/resource_addressed_execution_kernel.hpp>
#include <manyforge_core/sqlite_operation_journal.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>
#include <manyforge_roboplan/retained_execution_state_sink.hpp>
#include <manyforge_roboplan/retained_gripper_command_backend.hpp>
#include <manyforge_roboplan/retained_native_component_owner.hpp>
#include <manyforge_roboplan/retained_native_gripper_runtime.hpp>
#include <manyforge_roboplan/retained_native_gripper_route_publication_owner.hpp>
#include <manyforge_roboplan/retained_observed_state_channel.hpp>
#include <manyforge_roboplan/roboplan_execution_backend.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace manyforge::roboplan_backend {

class RetainedObservedStateChannelTestAccess final {
public:
  static std::unique_lock<std::mutex>
  lock_route(RetainedObservedStateChannel &channel) {
    return std::unique_lock<std::mutex>(channel.route_rebind_mutex_);
  }
};

namespace {

using namespace manyforge::core;
using namespace std::chrono_literals;

[[nodiscard]] RuntimeParticipantBinding
make_binding(CoordinateKind kind, std::string target = "gripper.target",
             bool component_local = false, std::string route = "route.native") {
  const bool angular = kind == CoordinateKind::kAngular;
  const std::vector<std::string> joints =
      angular ? std::vector<std::string>{"joint_1", "joint_2"}
              : std::vector<std::string>{"finger_joint", "finger_mimic_joint"};
  std::vector<RuntimeResourceBindingDTO> resources;
  const std::size_t direct_resource_count = angular ? joints.size() : 1U;
  for (std::size_t index = 0U; index < direct_resource_count; ++index) {
    resources.push_back(RuntimeResourceBindingDTO{
        ResourceRoleId(angular ? "arm_joints" : "finger"),
        ResourceId(angular ? "resource.joint." + std::to_string(index + 1U)
                           : "resource.finger"),
        component_local ? std::nullopt
                        : std::optional<std::string>(joints[index]),
        component_local ? "resource.finger" : joints[index], kind});
  }
  std::vector<RuntimeCapabilityProviderPathDTO> supporting_paths;
  if (angular) {
    supporting_paths.push_back(RuntimeCapabilityProviderPathDTO{
        CapabilityId("planning.motion.joint_goal"),
        CapabilityDefinitionRevision(
            "sha256:"
            "0f6cb2fda7e1d05f20a5867203b84c0e0427440f4b11657941a907c2e2a23dce"),
        CapabilityProviderBindingId("planner-binding"),
        CapabilityProviderBindingRevision("planner-binding-revision"),
        ProviderInstanceId("planner-provider"),
        ProviderManifestId("planner-provider-manifest"),
        ProviderManifestRevision("planner-provider-revision"),
        RuntimeUnitId("runtime.native"), RuntimeEpoch("runtime-epoch.1")});
  }
  RuntimeParticipantBindingDTO value{
      ProgramId("program.native"),
      ProgramRevision("program-revision"),
      ProgramBindingId("program-binding"),
      RetainedBundleId("bundle"),
      GraphRevision("graph"),
      ComponentRoleId("robot"),
      TargetRoleId(angular ? "arm" : "gripper"),
      ControllableComponentId("robot.1"),
      ComponentTypeId("aggregate.robot"),
      ComponentTypeRevision("aggregate-revision"),
      CommandTargetId(std::move(target)),
      CapabilityId(angular ? "execution.trajectory.execute"
                           : "gripper.command_position"),
      CapabilityDefinitionRevision("capability-revision"),
      std::nullopt,
      {ProgramModelRoleBindingDTO{ModelRoleId("body"),
                                  ModelInstanceId("robot.model"),
                                  ModelRevision("model-revision")}},
      component_local
          ? std::vector<RuntimeModelGroupBindingDTO>{}
          : std::vector<
                RuntimeModelGroupBindingDTO>{RuntimeModelGroupBindingDTO{
                ModelGroupRoleId(angular ? "arm_group" : "gripper_group"),
                ModelInstanceId("robot.model"), angular ? "arm" : "gripper",
                joints}},
      {ProgramFrameRoleBindingDTO{FrameRoleId("base"),
                                  ModelInstanceId("robot.model"),
                                  ModelRevision("model-revision"), "base"},
       ProgramFrameRoleBindingDTO{FrameRoleId("tcp"),
                                  ModelInstanceId("robot.model"),
                                  ModelRevision("model-revision"), "tool0"}},
      std::move(resources),
      {},
      RuntimeControlPathDTO{
          ControlDomainId(angular ? "domain.arm" : "domain.gripper"),
          ControlDomainRevision("domain-revision"),
          ControlBindingId(angular ? "binding.arm" : "binding.gripper"),
          ControlBindingRevision("binding-revision"),
          ControlInterfaceContractId(angular ? "trajectory.position"
                                             : "gripper.position"),
          ControlInterfaceRevision("interface-revision"),
          ControllerTopologyId("topology.native"),
          TopologyRevision("topology-revision"),
          ControlRouteId(std::move(route)),
          ActivationSetId("activation.native"),
          ActiveControlConfigurationId("active.native"),
          ActiveControlConfigurationGeneration(1U),
          ProviderInstanceId("provider.native"),
          ProviderManifestId("provider-manifest.native"),
          ProviderManifestRevision("provider-manifest-revision"),
          RuntimeUnitId("runtime.native"),
          RuntimeEpoch("runtime-epoch.1"),
          StateAuthorityId("state.native"),
          StateAuthorityClass::kPredicted,
          StopEvidencePolicyId("stop.native"),
          StopEvidencePolicyRevision("stop-revision"),
          ContainmentGranularity::kResource,
          angular ? std::optional(RuntimeScaleRouteQualificationDTO{
                        ControlRouteId("route.native"),
                        ScalingDomainId("scale.native"),
                        EndpointRoleId("runtime_scale_command"),
                        EndpointRoleId("runtime_scale_state"), std::nullopt,
                        RuntimeScaleAcknowledgementKind::kAppliedState, 4.0,
                        100'000'000})
                  : std::nullopt},
      std::nullopt,
      std::move(supporting_paths),
      {RuntimeUnitEpochBindingDTO{RuntimeUnitId("runtime.native"),
                                  RuntimeEpoch("runtime-epoch.1")}}};
  auto sealed = verify_runtime_participant_binding(std::move(value));
  if (!sealed.ok()) {
    throw std::logic_error(sealed.error().message);
  }
  return std::move(sealed.value());
}

[[nodiscard]] CoordinatePositionDTO tagged_position(CoordinateKind kind,
                                                    double value) {
  return kind == CoordinateKind::kAngular
             ? CoordinatePositionDTO{AngularPositionDTO{value}}
             : CoordinatePositionDTO{LinearPositionDTO{value}};
}

[[nodiscard]] CoordinateVelocityDTO tagged_velocity(CoordinateKind kind,
                                                    double value) {
  return kind == CoordinateKind::kAngular
             ? CoordinateVelocityDTO{AngularVelocityDTO{value}}
             : CoordinateVelocityDTO{LinearVelocityDTO{value}};
}

class FakeRetainedStateChannel final : public IRetainedObservedStateChannel {
public:
  explicit FakeRetainedStateChannel(const RuntimeParticipantBinding &binding,
                                    std::vector<double> initial)
      : context_([&] {
          auto context = make_observed_state_query_context(binding, 0);
          if (!context.ok() ||
              initial.size() != context.value().resources.size()) {
            throw std::logic_error("invalid fake retained-state input");
          }
          return std::move(context.value());
        }()),
        state_{context_.controllable_component_id,
               context_.command_target_id,
               context_.control_domain_id,
               context_.control_domain_revision,
               context_.control_binding_id,
               context_.control_binding_revision,
               context_.controller_topology_id,
               context_.topology_revision,
               context_.control_route_id,
               context_.active_control_configuration_id,
               context_.active_control_configuration_generation,
               context_.resources,
               context_.joint_names,
               {},
               std::nullopt} {
    for (std::size_t index = 0U; index < initial.size(); ++index) {
      state_.positions.push_back(
          tagged_position(context_.coordinate_kinds[index], initial[index]));
    }
  }

  RetainedStateProjectionResult
  publish_state(std::span<const double> positions_si,
                std::span<const double> velocities_si,
                std::int64_t source_timestamp_ns,
                std::int64_t received_timestamp_ns) override {
    std::unique_lock lock(mutex_);
    ++publish_calls_;
    if (block_publish_) {
      publish_entered_ = true;
      publish_cv_.notify_all();
      publish_cv_.wait_for(lock, 2s, [&] { return release_publish_; });
    }
    if (positions_si.size() != context_.resources.size() ||
        (!velocities_si.empty() &&
         velocities_si.size() != context_.resources.size())) {
      return failure(RetainedStateProjectionStatus::kRejected,
                     "fake_state_shape_mismatch");
    }
    state_.positions.clear();
    for (std::size_t index = 0U; index < positions_si.size(); ++index) {
      state_.positions.push_back(tagged_position(
          context_.coordinate_kinds[index], positions_si[index]));
    }
    if (velocities_si.empty()) {
      state_.velocities.reset();
    } else {
      state_.velocities.emplace();
      for (std::size_t index = 0U; index < velocities_si.size(); ++index) {
        state_.velocities->push_back(tagged_velocity(
            context_.coordinate_kinds[index], velocities_si[index]));
      }
    }
    source_timestamp_ns_ = source_timestamp_ns;
    received_timestamp_ns_ = received_timestamp_ns;
    ++sequence_;
    return {RetainedStateProjectionStatus::kApplied, std::nullopt};
  }

  RetainedStateProjectionResult
  project_latest(std::int64_t, RetainedStateProjectionMode) override {
    std::lock_guard lock(mutex_);
    ++project_calls_;
    if (projection_fail_) {
      return failure(RetainedStateProjectionStatus::kFailed,
                     "fake_projection_failed");
    }
    if (projection_busy_count_ > 0) {
      --projection_busy_count_;
      return failure(RetainedStateProjectionStatus::kBusy,
                     "fake_projection_busy");
    }
    ++project_applied_calls_;
    return {RetainedStateProjectionStatus::kApplied, std::nullopt};
  }

  RetainedStateProjectionResult
  publish_state_bounded(std::span<const double> positions_si,
                        std::span<const double> velocities_si,
                        std::int64_t source_timestamp_ns,
                        std::int64_t received_timestamp_ns, std::int64_t,
                        std::stop_token cancellation) override {
    if (cancellation.stop_requested()) {
      return {RetainedStateProjectionStatus::kFailed,
              ErrorDTO{"invalid_request",
                       "fake publication cancelled",
                       {{"reason", "fake_publication_cancelled"}}}};
    }
    {
      std::lock_guard lock(mutex_);
      if (block_publish_) {
        return {RetainedStateProjectionStatus::kFailed,
                ErrorDTO{"invalid_request",
                         "fake publication blocked",
                         {{"reason", "fake_publication_deadline"}}}};
      }
    }
    return publish_state(positions_si, velocities_si, source_timestamp_ns,
                         received_timestamp_ns);
  }

  RetainedStateProjectionResult
  publish_and_project(std::span<const double> positions_si,
                      std::span<const double> velocities_si,
                      std::int64_t source_timestamp_ns,
                      std::int64_t received_timestamp_ns,
                      RetainedStateProjectionMode mode) override {
    auto published = publish_state(positions_si, velocities_si,
                                   source_timestamp_ns, received_timestamp_ns);
    return published.applied() ? project_latest(received_timestamp_ns, mode)
                               : published;
  }

  Result<ObservedStateSnapshotDTO> current_state(std::int64_t) const override {
    std::lock_guard lock(mutex_);
    return Result<ObservedStateSnapshotDTO>::Ok(ObservedStateSnapshotDTO{
        context_.controllable_component_id,
        context_.command_target_id,
        context_.control_domain_id,
        context_.control_domain_revision,
        context_.control_binding_id,
        context_.control_binding_revision,
        context_.provider_instance_id,
        context_.provider_manifest_id,
        context_.provider_manifest_revision,
        context_.controller_topology_id,
        context_.topology_revision,
        context_.control_route_id,
        context_.active_control_configuration_id,
        context_.active_control_configuration_generation,
        context_.resources,
        sequence_,
        context_.joint_names,
        state_.positions,
        state_.velocities,
        "fake.native.state",
        context_.runtime_epoch,
        ObservedStateSourceKind::kPredictedState,
        source_timestamp_ns_,
        received_timestamp_ns_,
        context_.state_authority_id,
        context_.state_authority_class,
        1U,
        context_.freshness_budget_ns,
        true});
  }

  Result<ObservedStateSnapshotDTO>
  current_state_bounded(std::int64_t now_ns, std::int64_t,
                        std::stop_token cancellation) const override {
    if (cancellation.stop_requested()) {
      return Result<ObservedStateSnapshotDTO>::Err(
          ErrorDTO{"invalid_request",
                   "fake read cancelled",
                   {{"reason", "fake_read_cancelled"}}});
    }
    {
      std::lock_guard lock(mutex_);
      if (block_read_) {
        return Result<ObservedStateSnapshotDTO>::Err(
            ErrorDTO{"invalid_request",
                     "fake read blocked",
                     {{"reason", "fake_read_deadline"}}});
      }
    }
    return current_state(now_ns);
  }

  Result<ControlledStateDTO> current_controlled_state() const override {
    std::unique_lock lock(mutex_);
    const auto snapshot = state_;
    if (block_read_) {
      read_entered_ = true;
      ++read_entered_count_;
      read_cv_.notify_all();
      read_cv_.wait_for(lock, 2s, [&] { return release_read_; });
    }
    return Result<ControlledStateDTO>::Ok(snapshot);
  }

  ObservedStateQueryContextDTO query_context() const override {
    return context_;
  }

  const AddressedObservedStatePublication *
  addressed_publication() const noexcept override {
    return nullptr;
  }

  void make_projection_busy(int count) {
    std::lock_guard lock(mutex_);
    projection_busy_count_ = count;
  }

  void block_publish() {
    std::lock_guard lock(mutex_);
    block_publish_ = true;
    publish_entered_ = false;
    release_publish_ = false;
  }

  [[nodiscard]] bool wait_for_publish() const {
    std::unique_lock lock(mutex_);
    return publish_cv_.wait_for(lock, 2s, [&] { return publish_entered_; });
  }

  void release_publish() {
    {
      std::lock_guard lock(mutex_);
      release_publish_ = true;
      block_publish_ = false;
    }
    publish_cv_.notify_all();
  }

  void block_read() {
    std::lock_guard lock(mutex_);
    block_read_ = true;
    read_entered_ = false;
    read_entered_count_ = 0;
    release_read_ = false;
  }

  [[nodiscard]] bool wait_for_read() const {
    std::unique_lock lock(mutex_);
    return read_cv_.wait_for(lock, 2s, [&] { return read_entered_; });
  }

  [[nodiscard]] bool
  wait_for_read_count(const int count,
                      const std::chrono::milliseconds timeout) const {
    std::unique_lock lock(mutex_);
    return read_cv_.wait_for(lock, timeout,
                             [&] { return read_entered_count_ >= count; });
  }

  void release_read() {
    {
      std::lock_guard lock(mutex_);
      release_read_ = true;
      block_read_ = false;
    }
    read_cv_.notify_all();
  }

  [[nodiscard]] int publish_calls() const {
    std::lock_guard lock(mutex_);
    return publish_calls_;
  }

  [[nodiscard]] int project_calls() const {
    std::lock_guard lock(mutex_);
    return project_calls_;
  }

  [[nodiscard]] int project_applied_calls() const {
    std::lock_guard lock(mutex_);
    return project_applied_calls_;
  }

private:
  [[nodiscard]] static RetainedStateProjectionResult
  failure(RetainedStateProjectionStatus status, std::string reason) {
    return {status, ErrorDTO{std::string(to_string(ErrorCode::kInvalidRequest)),
                             "injected retained-state result",
                             {{"reason", std::move(reason)}}}};
  }

  ObservedStateQueryContextDTO context_;
  mutable std::mutex mutex_;
  mutable std::condition_variable publish_cv_;
  mutable std::condition_variable read_cv_;
  ControlledStateDTO state_;
  std::uint64_t sequence_{};
  std::int64_t source_timestamp_ns_{};
  std::int64_t received_timestamp_ns_{};
  int publish_calls_{};
  int project_calls_{};
  int project_applied_calls_{};
  int projection_busy_count_{};
  bool projection_fail_{};
  bool block_publish_{};
  bool publish_entered_{};
  bool release_publish_{};
  mutable bool block_read_{};
  mutable bool read_entered_{};
  mutable int read_entered_count_{};
  mutable bool release_read_{};
};

class RejectingScheduler final : public IScheduler {
public:
  ScheduleHandle schedule(TimePoint, std::function<void()>,
                          SchedulerPriority) override {
    throw std::runtime_error("injected schedule failure");
  }
  void cancel(ScheduleHandle) override {}
};

class InlineScheduler final : public IScheduler {
public:
  ScheduleHandle schedule(TimePoint, std::function<void()> callback,
                          SchedulerPriority) override {
    const auto handle = next_++;
    callback();
    return handle;
  }
  void cancel(ScheduleHandle) override {}

private:
  ScheduleHandle next_{1U};
};

class InlineThenThrowScheduler final : public IScheduler {
public:
  ScheduleHandle schedule(TimePoint, std::function<void()> callback,
                          SchedulerPriority) override {
    callback();
    throw std::runtime_error("injected post-callback failure");
  }
  void cancel(ScheduleHandle) override {}
};

class AutoAdvanceClock final : public IClock {
public:
  TimePoint now() const override {
    return current_.fetch_add(10'000'000, std::memory_order_acq_rel);
  }

private:
  mutable std::atomic<TimePoint> current_{1};
};

[[nodiscard]] GripperCommandRequestDTO
gripper_request(const RuntimeParticipantBinding &binding, double target,
                double duration_s) {
  const auto &value = binding.value();
  return GripperCommandRequestDTO{
      value.command_target_id.value(),
      value.model_groups.empty()
          ? std::string{}
          : value.model_groups.front().compiled_group_name,
      value.resources.front().compiled_resource_name,
      target,
      duration_s,
      8.0,
      "position",
      value.resources.front().coordinate_kind == CoordinateKind::kAngular
          ? "angular"
          : "linear",
      value.control_path->state_authority_id.value(),
      "position_reached",
      "optional",
      "resolved_binding",
      {"native_state"},
      "commanded"};
}

[[nodiscard]] RetainedGripperPositionLimits
gripper_limits(const RuntimeParticipantBinding &binding) {
  return RetainedGripperPositionLimits{
      binding.value().resources.front().coordinate_kind, -1.0, 1.0};
}

[[nodiscard]] ResolvedParticipantStopStageDTO
gripper_stop_stage(const RuntimeParticipantBinding &binding,
                   const bool controller_goal_cancel = false) {
  const auto &value = binding.value();
  const auto &path = *value.control_path;
  const StopBudgetPhaseId command_phase("gripper.hard.command");
  const std::vector<ResourceId> resources{value.resources.front().resource_id};
  const ResolvedStopRuntimeTargetDTO target{
      path.provider_instance_id,
      path.provider_manifest_id,
      path.provider_manifest_revision,
      path.provider_runtime_unit_id,
      ProductId("product.native"),
      ProductContractId("contract.native"),
      ProductContractRevision("contract.revision"),
      ContentDigest("target.digest"),
      path.provider_runtime_epoch,
      path.provider_runtime_epoch,
      EndpointRoleId("state.stop"),
      "state.stop",
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt};
  auto evidence_target = target;
  if (controller_goal_cancel) {
    evidence_target.endpoint_role_id.reset();
    evidence_target.logical_endpoint_symbol.reset();
  }
  const ResolvedStopEvidenceSourceDTO evidence_source{
      controller_goal_cancel ? StopEvidenceSourceKind::kControllerGoalStatus
                             : StopEvidenceSourceKind::kBindingStateAuthority,
      evidence_target,
      controller_goal_cancel ? std::optional<StateAuthorityId>{}
                             : std::optional(path.state_authority_id),
      controller_goal_cancel ? std::optional<StateAuthorityClass>{}
                             : std::optional(path.state_authority_class),
      std::nullopt};
  const ResolvedStopEscalationStageDTO resolved_stage{
      0U,
      StopEscalationStageId(controller_goal_cancel ? "stage.gripper.cancel"
                                                   : "stage.gripper.hard"),
      controller_goal_cancel ? StopEscalationStageKind::kNominal
                             : StopEscalationStageKind::kSoftwareHard,
      ContainmentGranularity::kResource,
      StopContainmentCommandDTO{
          command_phase,
          controller_goal_cancel
              ? StopContainmentCommandKind::kControllerGoalCancel
              : StopContainmentCommandKind::kRegistered,
          std::nullopt,
          controller_goal_cancel
              ? std::optional<RegisteredStopCommandKindId>{}
              : std::optional<
                    RegisteredStopCommandKindId>{RegisteredStopCommandKindId(
                    "manyforge.stop.software_hard")}},
      std::nullopt,
      ResolvedStopCommandQualificationDTO{},
      target,
      ResolvedStopResourceContainmentScopeDTO{path.activation_set_id,
                                              resources},
      StopEvidenceConjunction::kAllOf,
      {ResolvedStopEvidenceRequirementDTO{
          StopEvidenceRequirementDTO{
              StopEvidenceItemId(controller_goal_cancel
                                     ? "gripper.goal-terminal"
                                     : "gripper.rest"),
              StopBudgetPhaseId("gripper.hard.rest"),
              controller_goal_cancel
                  ? StopEvidenceRequirementKind::kControllerGoalTerminal
                  : StopEvidenceRequirementKind::kMotionRest,
              std::nullopt, std::nullopt,
              StopEvidenceSourceRequirementDTO{
                  controller_goal_cancel
                      ? StopEvidenceSourceKind::kControllerGoalStatus
                      : StopEvidenceSourceKind::kBindingStateAuthority,
                  100'000'000,
                  controller_goal_cancel
                      ? std::optional<EndpointRoleId>{}
                      : std::optional(EndpointRoleId("state.stop")),
                  controller_goal_cancel
                      ? std::vector<StateAuthorityClass>{}
                      : std::vector{path.state_authority_class}},
              StopEvidenceObservationDTO{
                  controller_goal_cancel
                      ? StopEvidenceObservationKind::kPostCommandSample
                      : StopEvidenceObservationKind::kPostCommandDwell,
                  controller_goal_cancel ? std::optional<std::int64_t>{}
                                         : std::optional<std::int64_t>{0}}},
          evidence_source}}};
  const OperationId operation_id("operation.gripper.stop");
  const OperationRevision operation_revision(1U);
  const OperationParticipantId participant_id("participant.gripper");
  return ResolvedParticipantStopStageDTO{
      StopId("stop.gripper"),
      StopGeneration(1U),
      operation_id,
      operation_revision,
      participant_id,
      ControllerGoalId("goal.gripper"),
      ControllerGoalGeneration(1U),
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
      ContainmentGranularity::kResource,
      path.stop_evidence_policy_id,
      path.stop_evidence_policy_revision,
      PreparedParticipantStopPlanDTO{
          ContentDigest("plan.digest"),
          ResolvedStopBasePlanId("base.gripper"),
          operation_id,
          operation_revision,
          participant_id,
          value.program_id,
          value.program_revision,
          value.program_binding_id,
          ProgramRunId("run.gripper"),
          std::nullopt,
          ParticipantStopProfilePurposeDTO::kOrdinary,
          std::nullopt,
          StopDeadlineBudgetDTO{
              1'000'000,
              100'000,
              {{command_phase, 500'000},
               {StopBudgetPhaseId("gripper.hard.rest"), 900'000}}},
          {PreparedStopStageCommandDTO{resolved_stage, std::nullopt,
                                       std::nullopt}}},
      0U,
      ContentDigest("scope.digest")};
}

TEST(RetainedGripperCommandBackendTest,
     ExactPreparedGoalCancelIsANominalContainedStop) {
  const auto binding = make_binding(CoordinateKind::kLinear);
  const auto stage = gripper_stop_stage(binding, true);
  const RemainingStopBudgetDTO budget{
      stage.stop_id,
      stage.stop_generation,
      1'000'000,
      {{StopBudgetPhaseId("gripper.hard.command"), 500'000},
       {StopBudgetPhaseId("gripper.hard.rest"), 900'000}}};
  auto channel = std::make_shared<FakeRetainedStateChannel>(
      binding, std::vector<double>{0.0});
  FakeClock clock;
  ManualScheduler scheduler;
  RetainedGripperCommandBackend backend(
      binding, channel, gripper_limits(binding), clock, scheduler);
  auto request = gripper_request(binding, 0.02, 1.0);
  request.addressed_identity = AddressedGripperCommandIdentityDTO{
      stage.operation_id, stage.operation_revision, stage.participant_id,
      stage.controller_goal_id, stage.controller_goal_generation};
  const auto command = backend.command_gripper(request);
  ASSERT_TRUE(command.accepted);

  const auto result =
      backend.TranslateStopStage(stage, budget, std::stop_token{});

  EXPECT_EQ(result.attempt_disposition,
            StopStageAttemptDispositionDTO::kDispatched);
  EXPECT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded);
  EXPECT_FALSE(result.error.has_value());
  ASSERT_EQ(result.evidence_items.size(), 1U);
  EXPECT_EQ(result.evidence_items.front().required_class,
            StopEvidenceRequirementKind::kControllerGoalTerminal);
  EXPECT_EQ(result.evidence_items.front().state,
            StopEvidenceItemStateDTO::kSatisfied);
  EXPECT_TRUE(result.controller_acknowledgement_sequence.has_value());
  EXPECT_EQ(backend.get_gripper_status("gripper.target").state,
            GripperCommandState::kIdle);
}

TEST(RetainedGripperCommandBackendTest,
     StopStageReadAndPublishBlockersReturnBoundedTerminalFailures) {
  const auto binding = make_binding(CoordinateKind::kLinear);
  const auto stage = gripper_stop_stage(binding);
  const RemainingStopBudgetDTO budget{
      stage.stop_id,
      stage.stop_generation,
      1'000'000,
      {{StopBudgetPhaseId("gripper.hard.command"), 500'000},
       {StopBudgetPhaseId("gripper.hard.rest"), 900'000}}};
  FakeClock clock;
  ManualScheduler scheduler;

  {
    auto channel = std::make_shared<FakeRetainedStateChannel>(
        binding, std::vector<double>{0.0});
    channel->block_read();
    RetainedGripperCommandBackend backend(
        binding, channel, gripper_limits(binding), clock, scheduler);
    const auto result =
        backend.TranslateStopStage(stage, budget, std::stop_token{});
    EXPECT_EQ(result.attempt_disposition,
              StopStageAttemptDispositionDTO::kNotDispatched);
    EXPECT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kFailed);
    ASSERT_TRUE(result.error.has_value());
    EXPECT_EQ(result.error->details.at("reason"),
              "stop_evidence_source_unavailable");
  }

  {
    auto channel = std::make_shared<FakeRetainedStateChannel>(
        binding, std::vector<double>{0.0});
    channel->block_publish();
    RetainedGripperCommandBackend backend(
        binding, channel, gripper_limits(binding), clock, scheduler);
    const auto result =
        backend.TranslateStopStage(stage, budget, std::stop_token{});
    EXPECT_EQ(result.attempt_disposition,
              StopStageAttemptDispositionDTO::kDispatched);
    EXPECT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kFailed);
    ASSERT_TRUE(result.error.has_value());
    EXPECT_EQ(result.error->details.at("reason"), "fake_publication_deadline");
  }
}

TEST(RetainedObservedStateChannelTest,
     BoundedReadAndPublishFailClosedUnderRouteContention) {
  auto binding = make_binding(CoordinateKind::kLinear, "gripper.target", true);
  FakeClock clock;
  clock.set(100);
  RetainedObservedStateChannel channel(
      binding, nullptr,
      RetainedObservedStateChannelConfig{
          ObservedStateSourceKind::kPredictedState, "native.predicted", 0, 1U},
      &clock);
  const std::array<double, 1U> initial_position{0.0};
  const std::array<double, 1U> initial_velocity{0.0};
  ASSERT_TRUE(
      channel.publish_state(initial_position, initial_velocity, 100, 100)
          .applied());

  std::promise<void> route_locked;
  std::promise<void> release_route;
  auto route_locked_future = route_locked.get_future();
  auto release_route_future = release_route.get_future();
  std::thread holder(
      [&channel, route_locked = std::move(route_locked),
       release_route_future = std::move(release_route_future)]() mutable {
        auto route_lock =
            RetainedObservedStateChannelTestAccess::lock_route(channel);
        route_locked.set_value();
        release_route_future.wait();
      });
  route_locked_future.wait();
  const auto started = std::chrono::steady_clock::now();
  const std::array<double, 1U> next_position{0.1};
  const auto published = channel.publish_state_bounded(
      next_position, initial_velocity, 100, 100, 1'000, std::stop_token{});
  const auto read =
      channel.current_state_bounded(100, 1'000, std::stop_token{});
  const auto elapsed = std::chrono::steady_clock::now() - started;
  release_route.set_value();
  holder.join();

  EXPECT_FALSE(published.applied());
  ASSERT_TRUE(published.error.has_value());
  EXPECT_EQ(published.error->details.at("reason"), "retained_state_route_busy");
  EXPECT_FALSE(read.ok());
  EXPECT_EQ(read.error().details.at("reason"), "retained_state_route_busy");
  EXPECT_LT(elapsed, 100ms);
}

TEST(RetainedObservedStateChannelTest,
     SameOwnerRouteRebindRepublishesSeedAndNoopStaysReadable) {
  const auto binding = make_binding(CoordinateKind::kLinear, "gripper.target",
                                    true, "route.native");
  const auto rebound = make_binding(CoordinateKind::kLinear, "gripper.target",
                                    true, "route.successor");
  RetainedObservedStateChannel channel(
      binding, nullptr,
      RetainedObservedStateChannelConfig{
          ObservedStateSourceKind::kPredictedState, "native.predicted", 0, 1U});
  const std::array<double, 1U> position{0.0};
  const std::array<double, 1U> velocity{0.0};
  ASSERT_TRUE(channel.publish_state(position, velocity, 100, 100).applied());
  ASSERT_TRUE(channel.RebindRoute(binding, 101).ok());
  const auto initial = channel.current_state(101);
  ASSERT_TRUE(initial.ok()) << initial.error().message;
  EXPECT_EQ(initial.value().control_route_id, ControlRouteId("route.native"));
  ASSERT_TRUE(channel.RebindRoute(rebound, 102).ok());

  const auto current = channel.current_state(102);
  ASSERT_TRUE(current.ok()) << current.error().message;
  EXPECT_EQ(current.value().control_route_id,
            ControlRouteId("route.successor"));
  EXPECT_DOUBLE_EQ(
      std::get<LinearPositionDTO>(current.value().positions.front()).value_m,
      0.0);
  EXPECT_TRUE(channel.wait_for_fresh_state(0ns));
}

TEST(RetainedObservedStateChannelTest,
     BoundStatePairsBindingAndSampleUnderOneRouteFence) {
  const auto binding = make_binding(CoordinateKind::kLinear, "gripper.target",
                                    true, "route.native");
  const auto rebound = make_binding(CoordinateKind::kLinear, "gripper.target",
                                    true, "route.successor");
  RetainedObservedStateChannel channel(
      binding, nullptr,
      RetainedObservedStateChannelConfig{
          ObservedStateSourceKind::kPredictedState, "native.predicted", 0, 1U});
  const std::array position{0.0}, velocity{0.0};
  ASSERT_TRUE(channel.publish_state(position, velocity, 100, 100).applied());

  // A binding read followed by a separate state read straddles an Apply route
  // publication: the pair crosses identities although both reads succeed.
  const auto split_binding = channel.binding();
  ASSERT_TRUE(channel.RebindRoute(rebound, 101).ok());
  const auto split_state = channel.current_state(101);
  ASSERT_TRUE(split_state.ok());
  EXPECT_NE(split_binding.value().control_path->control_route_id,
            split_state.value().control_route_id);

  const auto bound = channel.current_bound_state(101);
  ASSERT_TRUE(bound.ok()) << bound.error().message;
  EXPECT_EQ(bound.value().binding.value(), rebound.value());
  EXPECT_EQ(bound.value().state.control_route_id,
            ControlRouteId("route.successor"));

  // Readers concurrent with repeated route publication never observe a
  // binding from one route paired with a sample from another.
  std::atomic<bool> stop{false};
  std::thread publisher([&] {
    for (std::int64_t index = 0; index < 2000; ++index) {
      const auto &next = index % 2 == 0 ? binding : rebound;
      EXPECT_TRUE(channel.RebindRoute(next, 200 + index).ok());
    }
    stop.store(true, std::memory_order_release);
  });
  std::size_t reads = 0U;
  while (!stop.load(std::memory_order_acquire) || reads == 0U) {
    const auto current = channel.current_bound_state(5000);
    const bool paired =
        current.ok() &&
        current.value().binding.value().control_path->control_route_id ==
            current.value().state.control_route_id;
    EXPECT_TRUE(paired) << (current.ok() ? "crossed route identity"
                                         : current.error().message);
    if (!paired) break;
    ++reads;
  }
  publisher.join();
  EXPECT_GT(reads, 0U);
}

TEST(RetainedObservedStateChannelTest,
     MoreThan4096RouteChangesRetainOnlyCurrentBinding) {
  const auto first = make_binding(CoordinateKind::kLinear, "gripper.target",
                                  true, "route.first");
  const auto second = make_binding(CoordinateKind::kLinear, "gripper.target",
                                   true, "route.second");
  RetainedObservedStateChannel channel(
      first, nullptr,
      RetainedObservedStateChannelConfig{
          ObservedStateSourceKind::kPredictedState, "native.predicted", 0, 1U});
  const std::array position{0.0}, velocity{0.0};
  ASSERT_TRUE(channel.publish_state(position, velocity, 100, 100).applied());
  const auto captured = channel.binding();
  for (std::int64_t index = 1; index <= 5000; ++index) {
    const auto &next = index % 2 == 0 ? first : second;
    ASSERT_TRUE(channel.RebindRoute(next, 100 + index).ok());
    EXPECT_EQ(channel.binding().value(), next.value());
    const auto current = channel.current_state_bounded(100 + index, 101 + index,
                                                       std::stop_token{});
    ASSERT_TRUE(current.ok());
    EXPECT_EQ(current.value().control_route_id,
              next.value().control_path->control_route_id);
  }
  EXPECT_EQ(captured.value(), first.value());
}

TEST(RetainedObservedStateChannelTest,
     RenderChangeWakeIgnoresFreshnessOnlySamples) {
  const auto binding =
      make_binding(CoordinateKind::kLinear, "gripper.target", true);
  RetainedObservedStateChannel channel(
      binding, nullptr,
      RetainedObservedStateChannelConfig{
          ObservedStateSourceKind::kPredictedState, "native.predicted", 0, 1U});
  const std::array<double, 1U> position{0.0};
  const std::array<double, 1U> velocity{0.0};
  ASSERT_TRUE(channel.publish_state(position, velocity, 100, 100).applied());
  const auto before = channel.render_change_generation();
  EXPECT_FALSE(channel.wait_for_render_change(before, 0ns));

  ASSERT_TRUE(channel.publish_state(position, velocity, 101, 101).applied());
  EXPECT_EQ(channel.render_change_generation(), before);
  EXPECT_FALSE(channel.wait_for_render_change(before, 0ns));

  std::promise<bool> changed;
  auto changed_future = changed.get_future();
  std::thread waiter(
      [&] { changed.set_value(channel.wait_for_render_change(before, 1s)); });
  const std::array<double, 1U> next_position{0.1};
  ASSERT_TRUE(
      channel.publish_state(next_position, velocity, 102, 102).applied());
  EXPECT_TRUE(changed_future.get());
  waiter.join();
  EXPECT_GT(channel.render_change_generation(), before);
}

TEST(RetainedObservedStateChannelTest, SharedWakePublishesGripperOnlyChangesAndHasOnePendingToken) {
  auto wake = std::make_shared<RetainedObservationWake>();
  const auto config = RetainedObservedStateChannelConfig{
      ObservedStateSourceKind::kPredictedState, "native.predicted", 0, 1U};
  RetainedObservedStateChannel arm(make_binding(CoordinateKind::kLinear, "arm.target", true),
                                   nullptr, config, nullptr, wake);
  RetainedObservedStateChannel gripper(
      make_binding(CoordinateKind::kLinear, "gripper.target", true), nullptr, config, nullptr,
      wake);
  const std::array zero{0.0}, moved{0.025};
  ASSERT_TRUE(arm.publish_state(zero, zero, 100, 100).applied());
  ASSERT_TRUE(gripper.publish_state(zero, zero, 100, 100).applied());
  const auto before = arm.render_change_generation();
  ASSERT_TRUE(arm.publish_state(zero, zero, 101, 101).applied());
  EXPECT_EQ(arm.render_change_generation(), before);
  std::promise<bool> changed;
  auto changed_future = changed.get_future();
  std::jthread reader([&] { changed.set_value(arm.wait_for_render_change(before, 1s)); });
  ASSERT_TRUE(gripper.publish_state(moved, zero, 102, 102).applied());
  EXPECT_TRUE(changed_future.get());
  EXPECT_EQ(arm.render_change_generation(), gripper.render_change_generation());
  for (int sample = 0; sample < 100'000; ++sample) wake->notify();
  EXPECT_TRUE(wake->wait_after(before, 0ns));
  EXPECT_FALSE(wake->wait_after(wake->generation(), 0ns));
  RetainedObservationWake replacement;
  EXPECT_NE(replacement.incarnation(), wake->incarnation());
  EXPECT_EQ(replacement.generation(), 0U);
}

TEST(RetainedObservedStateChannelTest,
     PositionOnlyChangesAndInvalidationWakeWithoutGrantingRestEvidence) {
  const auto binding = make_binding(CoordinateKind::kLinear, "gripper.target", true);
  RetainedObservedStateChannel channel(
      binding, nullptr,
      RetainedObservedStateChannelConfig{
          ObservedStateSourceKind::kPredictedState, "native.predicted", 0, 1U});
  const auto names = channel.query_context().joint_names;
  const std::vector<double> positions(names.size(), 0.0), velocities(names.size(), 0.0);
  const auto source = ObservedRobotStateSourceKind::kPredictedState;
  ASSERT_TRUE(
      channel.publish_adapter_state(source, names, positions, velocities, 100, 100).complete);
  const auto before = channel.render_change_generation();
  auto moved = positions;
  moved.front() = 0.1;
  const auto without_velocity = channel.publish_adapter_state(source, names, moved, {}, 101, 101);
  ASSERT_TRUE(without_velocity.complete);
  EXPECT_FALSE(without_velocity.velocities_complete);
  EXPECT_GT(channel.render_change_generation(), before);
  const auto moved_generation = channel.render_change_generation();
  auto incomplete = moved;
  incomplete.front() = std::numeric_limits<double>::quiet_NaN();
  ASSERT_TRUE(channel.publish_adapter_state(source, names, incomplete, {}, 102, 102).published);
  EXPECT_GT(channel.render_change_generation(), moved_generation);
  const auto invalid_generation = channel.render_change_generation();
  ASSERT_TRUE(channel.publish_adapter_state(source, names, incomplete, {}, 103, 103).published);
  EXPECT_EQ(channel.render_change_generation(), invalid_generation);
  ASSERT_TRUE(channel.publish_adapter_state(source, names, moved, {}, 104, 104).complete);
  EXPECT_GT(channel.render_change_generation(), invalid_generation);
}

TEST(RetainedExecutionStateSinkTest,
     MotionProjectionNeverRepublishesTheAdapterSuppliedVector) {
  auto binding = make_binding(CoordinateKind::kAngular, "arm.target");
  auto channel = std::make_shared<FakeRetainedStateChannel>(
      binding, std::vector<double>{0.1, 0.2});
  FakeClock clock;
  RetainedExecutionStateSink sink(
      clock, {{binding.value().model_groups.front().compiled_group_name,
               channel, false}});

  const auto result = sink.apply_group_joint_positions(
      "arm", {9.0, 8.0}, ExecutionStateUpdateMode::kBestEffort);

  EXPECT_TRUE(result.applied());
  EXPECT_EQ(channel->publish_calls(), 0);
  EXPECT_EQ(channel->project_calls(), 1);
  EXPECT_DOUBLE_EQ(*sink.group_joint_position("arm", "joint_1"), 0.1);
}

TEST(RetainedExecutionStateSinkTest,
     ExplicitScalarWriterPublishesAndProjectsOneAddressedState) {
  auto binding = make_binding(CoordinateKind::kLinear, "gripper.target");
  auto channel = std::make_shared<FakeRetainedStateChannel>(
      binding, std::vector<double>{0.0});
  FakeClock clock;
  clock.set(123);
  RetainedExecutionStateSink sink(
      clock, {{binding.value().model_groups.front().compiled_group_name,
               channel, true}});

  const auto result = sink.apply_group_joint_position(
      "gripper", "finger_joint", 0.02, ExecutionStateUpdateMode::kBlocking);

  EXPECT_TRUE(result.applied());
  EXPECT_EQ(channel->publish_calls(), 1);
  EXPECT_EQ(channel->project_applied_calls(), 1);
  EXPECT_DOUBLE_EQ(*sink.group_joint_position("gripper", "finger_joint"), 0.02);
}

TEST(RetainedExecutionStateSinkTest,
     ConcurrentScalarWritersCannotLoseAnotherJointUpdate) {
  auto binding = make_binding(CoordinateKind::kAngular, "arm.target");
  auto channel = std::make_shared<FakeRetainedStateChannel>(
      binding, std::vector<double>{0.0, 0.0});
  FakeClock clock;
  clock.set(123);
  RetainedExecutionStateSink sink(
      clock, {{binding.value().model_groups.front().compiled_group_name,
               channel, true}});
  channel->block_read();

  std::optional<ExecutionStateSinkResult> first_result;
  std::optional<ExecutionStateSinkResult> second_result;
  std::thread first([&] {
    first_result = sink.apply_group_joint_position(
        "arm", "joint_1", 0.1, ExecutionStateUpdateMode::kBlocking);
  });
  const bool first_entered = channel->wait_for_read();
  EXPECT_TRUE(first_entered);
  std::thread second([&] {
    second_result = sink.apply_group_joint_position(
        "arm", "joint_2", 0.2, ExecutionStateUpdateMode::kBlocking);
  });

  EXPECT_FALSE(channel->wait_for_read_count(2, 50ms));
  channel->release_read();
  first.join();
  second.join();

  ASSERT_TRUE(first_result.has_value());
  ASSERT_TRUE(second_result.has_value());
  EXPECT_TRUE(first_result->applied());
  EXPECT_TRUE(second_result->applied());
  EXPECT_EQ(channel->publish_calls(), 2);
  EXPECT_DOUBLE_EQ(*sink.group_joint_position("arm", "joint_1"), 0.1);
  EXPECT_DOUBLE_EQ(*sink.group_joint_position("arm", "joint_2"), 0.2);
}

TEST(RetainedExecutionStateSinkTest,
     BindingShapeAndProjectionBackpressureFailClosed) {
  auto binding = make_binding(CoordinateKind::kAngular, "arm.target");
  auto channel = std::make_shared<FakeRetainedStateChannel>(
      binding, std::vector<double>{0.1, 0.2});
  FakeClock clock;
  RetainedExecutionStateSink sink(
      clock, {{binding.value().model_groups.front().compiled_group_name,
               channel, false}});

  EXPECT_EQ(sink.apply_group_joint_positions(
                    "other", {0.1, 0.2}, ExecutionStateUpdateMode::kBlocking)
                .status,
            ExecutionStateSinkStatus::kInvalidGroup);
  EXPECT_EQ(sink.apply_group_joint_positions(
                    "arm", {0.1}, ExecutionStateUpdateMode::kBlocking)
                .status,
            ExecutionStateSinkStatus::kInvalidPositions);
  channel->make_projection_busy(1);
  EXPECT_EQ(sink.apply_group_joint_positions(
                    "arm", {0.1, 0.2}, ExecutionStateUpdateMode::kBestEffort)
                .status,
            ExecutionStateSinkStatus::kBusy);
}

[[nodiscard]] TrajectoryDTO trajectory(const RuntimeParticipantBinding &binding,
                                       double duration_s = 0.02) {
  const auto &value = binding.value();
  std::vector<ResourceId> resources;
  std::vector<std::string> joint_names;
  for (const auto &resource : value.resources) {
    resources.push_back(resource.resource_id);
    joint_names.push_back(resource.compiled_resource_name);
  }
  return TrajectoryDTO{
      std::move(resources),
      std::move(joint_names),
      {TrajectoryWaypointDTO{0.0, {0.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}},
       TrajectoryWaypointDTO{duration_s, {0.2, -0.3}, {0.0, 0.0}, {0.0, 0.0}}},
      TrajectoryRuntimeScaleBoundsDTO{4.0, 16.0}};
}

[[nodiscard]] RuntimeParticipantBinding confirmation_source(
    const RuntimeParticipantBinding& command) {
  auto value = command.value();
  value.capability_id = CapabilityId("tool.gripper.confirmation.operator");
  value.control_path->control_binding_id = ControlBindingId("binding.confirmation");
  value.control_path->control_binding_revision = ControlBindingRevision("confirmation-revision");
  auto result = verify_runtime_participant_binding(std::move(value));
  if (!result.ok()) throw std::logic_error(result.error().message);
  return std::move(result.value());
}

TEST(RetainedGripperCommandBackendTest,
     ConfirmationAdmissionDeadlineIsSeparateFromRetainedEvidenceValidity) {
  auto binding = make_binding(CoordinateKind::kLinear);
  auto channel = std::make_shared<FakeRetainedStateChannel>(binding, std::vector<double>{0.0});
  FakeClock clock;
  ManualScheduler scheduler;
  RetainedGripperCommandBackend backend(binding, channel, gripper_limits(binding), clock,
                                        scheduler);
  const auto command = backend.command_gripper(gripper_request(binding, 0.02, 0.0));
  ASSERT_TRUE(command.accepted);
  scheduler.advance_to(clock.now());
  const auto before = backend.get_gripper_status("gripper.target");
  const auto source = confirmation_source(binding);
  GripperGraspObservationDTO observation{"gripper.target", command.command_id, command.generation,
                                         before.revision,  "operator-late",    true,
                                         clock.now(),      "station-a"};
  const auto deadline = clock.now() + 100;
  clock.advance(101);
  const auto expired =
      backend.record_grasp_observation(source, observation, 3'600'000'000'000LL, deadline);
  ASSERT_FALSE(expired.ok());
  EXPECT_EQ(expired.error().code, "control_freshness_expired");
  const auto after = backend.get_gripper_status("gripper.target");
  EXPECT_EQ(after.revision, before.revision);
  EXPECT_EQ(after.command_id, before.command_id);
  EXPECT_EQ(after.generation, before.generation);
  EXPECT_EQ(after.state, before.state);
  EXPECT_EQ(after.grasp_evidence, before.grasp_evidence);
  observation.observation_id = "operator-current";
  observation.observed_at_ns = clock.now();
  const auto current =
      backend.record_grasp_observation(source, observation, 3'600'000'000'000LL, clock.now());
  ASSERT_TRUE(current.ok());
  EXPECT_EQ(current.value().expires_at_ns, observation.observed_at_ns + 3'600'000'000'000LL);
}

TEST(RetainedGripperCommandBackendTest,
     ConfirmationIsExplicitCommandScopedAndPureReadsDoNotRenewIt) {
  auto binding = make_binding(CoordinateKind::kLinear);
  auto channel = std::make_shared<FakeRetainedStateChannel>(binding, std::vector<double>{0.0});
  FakeClock clock;
  ManualScheduler scheduler;
  RetainedGripperCommandBackend backend(binding, channel, gripper_limits(binding), clock,
                                        scheduler);
  const auto command = backend.command_gripper(gripper_request(binding, 0.02, 0.0));
  ASSERT_TRUE(command.accepted);
  const auto source = confirmation_source(binding);
  GripperGraspObservationDTO observation{
      "gripper.target", command.command_id,  command.generation, 1U, "operator-press-1", true,
      clock.now(),      "operator:station-a"};
  EXPECT_FALSE(
      backend
          .record_grasp_observation(source, observation, 1'000'000'000, clock.now() + 1'000'000'000)
          .ok());
  scheduler.advance_to(clock.now());
  EXPECT_FALSE(backend.get_gripper_status("gripper.target").grasp_evidence.has_value());
  observation.expected_status_revision = backend.get_gripper_status("gripper.target").revision;
  const auto result = backend.record_grasp_observation(source, observation, 1'000'000'000,
                                                       clock.now() + 1'000'000'000);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value().source_kind, GraspConfirmationSourceKind::kOperator);
  EXPECT_EQ(result.value().source_id, source.value().control_path->control_binding_id);
  EXPECT_EQ(result.value().source_epoch, RuntimeEpoch("runtime-epoch.1"));
  EXPECT_TRUE(result.value().observation.confirmed);
  const auto revision = backend.get_gripper_status("gripper.target").revision;
  const auto projections = channel->publish_calls();
  clock.advance(1'000'000'000);
  for (int index = 0; index < 1000; ++index) {
    const auto status = backend.get_gripper_status("gripper.target");
    ASSERT_EQ(status.grasp_evidence, result.value());
    EXPECT_FALSE(gripper_grasp_evidence_is_fresh(*status.grasp_evidence, clock.now()));
    EXPECT_EQ(status.revision, revision);
  }
  observation.observed_at_ns = clock.now();
  const auto duplicate = backend.record_grasp_observation(source, observation, 1'000'000'000,
                                                          clock.now() + 1'000'000'000);
  ASSERT_TRUE(duplicate.ok());
  EXPECT_EQ(duplicate.value(), result.value());
  EXPECT_EQ(backend.get_gripper_status("gripper.target").revision, revision);
  EXPECT_EQ(channel->publish_calls(), projections);
  observation.confirmed = false;
  EXPECT_FALSE(
      backend
          .record_grasp_observation(source, observation, 1'000'000'000, clock.now() + 1'000'000'000)
          .ok());
}

TEST(RetainedGripperCommandBackendTest, ConfirmationRejectsUnqualifiedCrossedAndExpiredSources) {
  auto binding = make_binding(CoordinateKind::kLinear);
  auto channel = std::make_shared<FakeRetainedStateChannel>(binding, std::vector<double>{0.0});
  FakeClock clock;
  ManualScheduler scheduler;
  RetainedGripperCommandBackend backend(binding, channel, gripper_limits(binding), clock,
                                        scheduler);
  const auto command = backend.command_gripper(gripper_request(binding, 0.02, 0.0));
  ASSERT_TRUE(command.accepted);
  scheduler.advance_to(clock.now());
  GripperGraspObservationDTO observation{
      "gripper.target",   command.command_id,
      command.generation, backend.get_gripper_status("gripper.target").revision,
      "operator-press-1", true,
      clock.now(),        "operator:station-a"};
  const auto source = confirmation_source(binding);
  EXPECT_FALSE(
      backend.record_grasp_observation(binding, observation, 100, clock.now() + 1'000'000'000)
          .ok());
  auto wrong_kind = source.value();
  wrong_kind.capability_id = CapabilityId("tool.gripper.confirmation.discrete_input");
  EXPECT_FALSE(backend
                   .record_grasp_observation(verify_runtime_participant_binding(wrong_kind).value(),
                                             observation, 100, clock.now() + 1'000'000'000)
                   .ok());
  auto crossed = source.value();
  crossed.control_path->active_control_configuration_generation =
      ActiveControlConfigurationGeneration(2);
  EXPECT_FALSE(backend
                   .record_grasp_observation(verify_runtime_participant_binding(crossed).value(),
                                             observation, 100, clock.now() + 1'000'000'000)
                   .ok());
  EXPECT_FALSE(
      backend.record_grasp_observation(source, observation, 0, clock.now() + 1'000'000'000).ok());
  observation.observed_at_ns = 1;
  EXPECT_FALSE(
      backend.record_grasp_observation(source, observation, 100, clock.now() + 1'000'000'000).ok());
  observation.observed_at_ns = 0;
  clock.advance(100);
  EXPECT_FALSE(
      backend.record_grasp_observation(source, observation, 100, clock.now() + 1'000'000'000).ok());
  observation.observed_at_ns = clock.now();
  observation.attribution.clear();
  EXPECT_FALSE(
      backend.record_grasp_observation(source, observation, 100, clock.now() + 1'000'000'000).ok());
  EXPECT_FALSE(backend.get_gripper_status("gripper.target").grasp_evidence.has_value());
}

TEST(RetainedGripperCommandBackendTest,
     ConfirmationRetainsOnlyTheCurrentCommandAndRejectsAnOlderObservation) {
  auto binding = make_binding(CoordinateKind::kLinear);
  auto channel = std::make_shared<FakeRetainedStateChannel>(binding, std::vector<double>{0.0});
  FakeClock clock;
  ManualScheduler scheduler;
  RetainedGripperCommandBackend backend(binding, channel, gripper_limits(binding), clock,
                                        scheduler);
  const auto source = confirmation_source(binding);
  GripperGraspObservationDTO previous;
  for (int index = 0; index < 1000; ++index) {
    const auto command = backend.command_gripper(gripper_request(binding, 0.02, 0.0));
    ASSERT_TRUE(command.accepted);
    scheduler.advance_to(clock.now());
    EXPECT_FALSE(backend.get_gripper_status("gripper.target").grasp_evidence.has_value());
    if (index != 0) {
      EXPECT_FALSE(
          backend.record_grasp_observation(source, previous, 100, clock.now() + 1'000'000'000)
              .ok());
    }
    GripperGraspObservationDTO observation{
        "gripper.target",   command.command_id,
        command.generation, backend.get_gripper_status("gripper.target").revision,
        "operator-press",   true,
        clock.now(),        "operator:station-a"};
    ASSERT_TRUE(
        backend.record_grasp_observation(source, observation, 100, clock.now() + 1'000'000'000)
            .ok());
    previous = observation;
    clock.advance(1);
  }
  previous.observation_id = "new-observation";
  const auto old_positive = previous;
  previous.expected_status_revision = backend.get_gripper_status("gripper.target").revision;
  previous.observed_at_ns = clock.now();
  previous.confirmed = false;
  const auto latest =
      backend.record_grasp_observation(source, previous, 100, clock.now() + 1'000'000'000);
  ASSERT_TRUE(latest.ok());
  EXPECT_FALSE(latest.value().observation.confirmed);
  auto replayed_positive = old_positive;
  replayed_positive.observation_id = "operator-press";
  replayed_positive.observed_at_ns = clock.now();
  EXPECT_FALSE(
      backend.record_grasp_observation(source, replayed_positive, 100, clock.now() + 1'000'000'000)
          .ok());
  --previous.observed_at_ns;
  previous.observation_id = "older-observation";
  EXPECT_FALSE(
      backend.record_grasp_observation(source, previous, 100, clock.now() + 1'000'000'000).ok());
  EXPECT_EQ(backend.get_gripper_status("gripper.target").grasp_evidence, latest.value());
}

TEST(RetainedGripperCommandBackendTest, ConfirmationClearsOnRouteReplacementAndOwnerInvalidation) {
  auto binding = make_binding(CoordinateKind::kLinear);
  auto channel = std::make_shared<FakeRetainedStateChannel>(binding, std::vector<double>{0.0});
  FakeClock clock;
  ManualScheduler scheduler;
  RetainedGripperCommandBackend backend(binding, channel, gripper_limits(binding), clock,
                                        scheduler);
  const auto source = confirmation_source(binding);
  const auto command = backend.command_gripper(gripper_request(binding, 0.02, 0.0));
  ASSERT_TRUE(command.accepted);
  scheduler.advance_to(clock.now());
  GripperGraspObservationDTO observation{
      "gripper.target",   command.command_id,
      command.generation, backend.get_gripper_status("gripper.target").revision,
      "operator-press",   true,
      clock.now(),        "operator:station-a"};
  ASSERT_TRUE(
      backend.record_grasp_observation(source, observation, 100, clock.now() + 1'000'000'000).ok());
  ASSERT_TRUE(backend.RebindRoute(binding, channel).ok());
  EXPECT_TRUE(backend.get_gripper_status("gripper.target").grasp_evidence.has_value());
  auto replacement =
      make_binding(CoordinateKind::kLinear, "gripper.target", false, "route.replacement");
  auto replacement_channel =
      std::make_shared<FakeRetainedStateChannel>(replacement, std::vector<double>{0.02});
  ASSERT_TRUE(backend.RebindRoute(replacement, replacement_channel).ok());
  EXPECT_FALSE(backend.get_gripper_status("gripper.target").grasp_evidence.has_value());
  EXPECT_FALSE(
      backend.record_grasp_observation(source, observation, 100, clock.now() + 1'000'000'000).ok());
  // Empty proof still has a prepared CAS: A -> B -> A must retire it.
  observation.expected_status_revision = backend.get_gripper_status("gripper.target").revision;
  const auto empty_revision = observation.expected_status_revision;
  ASSERT_TRUE(backend.RebindRoute(binding, channel).ok());
  ASSERT_TRUE(backend.RebindRoute(replacement, replacement_channel).ok());
  EXPECT_EQ(backend.get_gripper_status("gripper.target").revision, empty_revision + 2U);
  EXPECT_FALSE(backend
                   .record_grasp_observation(confirmation_source(replacement), observation, 100,
                                             clock.now() + 1'000'000'000)
                   .ok());
  ASSERT_TRUE(backend.RebindRoute(replacement, replacement_channel).ok());
  EXPECT_EQ(backend.get_gripper_status("gripper.target").revision, empty_revision + 2U);
  observation.expected_status_revision = backend.get_gripper_status("gripper.target").revision;
  ASSERT_TRUE(backend
                  .record_grasp_observation(confirmation_source(replacement), observation, 100,
                                            clock.now() + 1'000'000'000)
                  .ok());
  backend.invalidate_gripper_commands(GripperInvalidationReason::kReplacement);
  EXPECT_FALSE(backend.get_gripper_status("gripper.target").grasp_evidence.has_value());
  EXPECT_FALSE(backend
                   .record_grasp_observation(confirmation_source(replacement), observation, 100,
                                             clock.now() + 1'000'000'000)
                   .ok());
}

TEST(RetainedGripperCommandBackendTest,
     EvidenceInvalidationDoesNotChangeCommandsOrResurrectOnSameRoute) {
  auto binding = make_binding(CoordinateKind::kLinear);
  auto channel = std::make_shared<FakeRetainedStateChannel>(binding, std::vector<double>{0.0});
  FakeClock clock;
  ManualScheduler scheduler;
  RetainedGripperCommandBackend backend(binding, channel, gripper_limits(binding), clock,
                                        scheduler);
  const auto source = confirmation_source(binding);
  const auto command = backend.command_gripper(gripper_request(binding, 0.02, 0.0));
  ASSERT_TRUE(command.accepted);
  scheduler.advance_to(clock.now());
  GripperGraspObservationDTO observation{
      "gripper.target",   command.command_id,
      command.generation, backend.get_gripper_status("gripper.target").revision,
      "operator-press",   true,
      clock.now(),        "operator:station-a"};
  ASSERT_TRUE(
      backend.record_grasp_observation(source, observation, 100, clock.now() + 1'000'000'000).ok());
  const auto before = backend.get_gripper_status("gripper.target");
  backend.invalidate_grasp_evidence();
  auto after = backend.get_gripper_status("gripper.target");
  EXPECT_FALSE(after.grasp_evidence.has_value());
  EXPECT_EQ(after.revision, before.revision + 1U);
  EXPECT_EQ(after.command_id, before.command_id);
  EXPECT_EQ(after.generation, before.generation);
  EXPECT_EQ(after.state, before.state);
  EXPECT_EQ(after.observed_position, before.observed_position);
  backend.invalidate_grasp_evidence();
  EXPECT_EQ(backend.get_gripper_status("gripper.target").revision, after.revision + 1U);
  ASSERT_TRUE(backend.RebindRoute(binding, channel).ok());
  EXPECT_FALSE(backend.get_gripper_status("gripper.target").grasp_evidence.has_value());
  EXPECT_FALSE(
      backend.record_grasp_observation(source, observation, 100, clock.now() + 1'000'000'000).ok());

  // Evidence retirement is not a lifecycle close or a physical cancellation.
  const auto next = backend.command_gripper(gripper_request(binding, 0.01, 1.0));
  ASSERT_TRUE(next.accepted);
  backend.invalidate_grasp_evidence();
  clock.advance(1'000'000'000);
  scheduler.advance_to(clock.now());
  after = backend.get_gripper_status("gripper.target");
  EXPECT_EQ(after.command_id, next.command_id);
  EXPECT_EQ(after.state, GripperCommandState::kCompleted);
  EXPECT_DOUBLE_EQ(*after.observed_position, 0.01);
  observation.command_id = after.command_id;
  observation.generation = after.generation;
  observation.expected_status_revision = after.revision;
  observation.observed_at_ns = clock.now();
  backend.invalidate_grasp_evidence();
  ASSERT_TRUE(backend.RebindRoute(binding, channel).ok());
  EXPECT_FALSE(
      backend.record_grasp_observation(source, observation, 100, clock.now() + 1'000'000'000).ok());
}

TEST(RetainedGripperCommandBackendTest,
     ProgressesLinearStateOnControllerLaneWithoutObserverSideEffects) {
  auto binding = make_binding(CoordinateKind::kLinear);
  auto channel = std::make_shared<FakeRetainedStateChannel>(
      binding, std::vector<double>{0.0});
  FakeClock clock;
  ManualScheduler scheduler;
  RetainedGripperCommandBackend backend(
      binding, channel, gripper_limits(binding), clock, scheduler);

  const auto admitted =
      backend.command_gripper(gripper_request(binding, 0.02, 1.0));
  ASSERT_TRUE(admitted.accepted);
  EXPECT_EQ(scheduler.last_priority(), SchedulerPriority::ControllerCommand);
  const auto first = backend.get_gripper_status("gripper.target");
  EXPECT_EQ(first.revision,
            backend.get_gripper_status("gripper.target").revision);
  EXPECT_EQ(channel->publish_calls(), 0);

  clock.advance(500'000'000);
  scheduler.advance_to(clock.now());
  auto halfway = backend.get_gripper_status("gripper.target");
  EXPECT_EQ(halfway.state, GripperCommandState::kExecuting);
  EXPECT_NEAR(*halfway.current_position, 0.01, 1e-12);

  clock.advance(500'000'000);
  scheduler.advance_to(clock.now());
  const auto completed = backend.get_gripper_status("gripper.target");
  EXPECT_EQ(completed.state, GripperCommandState::kCompleted);
  EXPECT_EQ(completed.projection_state, GripperProjectionState::kApplied);
  const auto state = channel->current_controlled_state();
  ASSERT_TRUE(state.ok());
  ASSERT_EQ(state.value().positions.size(), 1U);
  EXPECT_DOUBLE_EQ(
      std::get<LinearPositionDTO>(state.value().positions[0]).value_m, 0.02);
}

TEST(
    RetainedGripperCommandBackendTest,
    ComponentLocalZeroDurationCommandEstablishesAddressedStateWithoutModelProjection) {
  auto binding = make_binding(CoordinateKind::kLinear, "gripper.target", true);
  auto channel = std::make_shared<RetainedObservedStateChannel>(
      binding, nullptr,
      RetainedObservedStateChannelConfig{
          ObservedStateSourceKind::kPredictedState, "logical-gripper", 0, 1U});
  FakeClock clock;
  InlineScheduler scheduler;
  RetainedGripperCommandBackend backend(binding, channel, std::nullopt, clock,
                                        scheduler, 1);

  EXPECT_FALSE(channel->current_controlled_state().ok());
  const auto result =
      backend.command_gripper(gripper_request(binding, 0.01, 0.0));

  ASSERT_TRUE(result.accepted);
  const auto status = backend.get_gripper_status("gripper.target");
  EXPECT_EQ(status.state, GripperCommandState::kCompleted);
  const auto state = channel->current_controlled_state();
  ASSERT_TRUE(state.ok()) << state.error().message;
  ASSERT_EQ(state.value().positions.size(), 1U);
  EXPECT_DOUBLE_EQ(
      std::get<LinearPositionDTO>(state.value().positions.front()).value_m,
      0.01);
  EXPECT_EQ(
      channel
          ->project_latest(clock.now(), RetainedStateProjectionMode::kBlocking)
          .status,
      RetainedStateProjectionStatus::kApplied);
}

TEST(RetainedGripperCommandBackendTest,
     ComponentLocalTimedCommandRequiresPreviouslyPublishedState) {
  auto binding = make_binding(CoordinateKind::kLinear, "gripper.target", true);
  auto channel = std::make_shared<RetainedObservedStateChannel>(
      binding, nullptr,
      RetainedObservedStateChannelConfig{
          ObservedStateSourceKind::kPredictedState, "logical-gripper", 0, 1U});
  FakeClock clock;
  ManualScheduler scheduler;
  RetainedGripperCommandBackend backend(binding, channel, std::nullopt, clock,
                                        scheduler);

  const auto result =
      backend.command_gripper(gripper_request(binding, 0.01, 0.5));

  ASSERT_FALSE(result.accepted);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->details.at("reason"), "observed_state_not_published");
}

TEST(RetainedGripperCommandBackendTest,
     RejectsCrossedIdentityAndUnitBeforeScheduling) {
  auto binding = make_binding(CoordinateKind::kLinear);
  auto channel = std::make_shared<FakeRetainedStateChannel>(
      binding, std::vector<double>{0.0});
  FakeClock clock;
  ManualScheduler scheduler;
  RetainedGripperCommandBackend backend(
      binding, channel, gripper_limits(binding), clock, scheduler);
  auto crossed = gripper_request(binding, 0.02, 1.0);
  crossed.actuation_kind = "angular";

  const auto preflight = backend.validate_gripper_command(crossed);
  ASSERT_FALSE(preflight.ok());
  EXPECT_EQ(preflight.error().code, "invalid_request");
  EXPECT_EQ(preflight.error().details.at("reason"), "gripper_invalid_command");

  const auto result = backend.command_gripper(crossed);

  ASSERT_FALSE(result.accepted);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->code, "gripper_invalid_command");
  EXPECT_EQ(channel->publish_calls(), 0);

  auto outside_limits = gripper_request(binding, 2.0, 1.0);
  const auto limit_preflight = backend.validate_gripper_command(outside_limits);
  ASSERT_FALSE(limit_preflight.ok());
  EXPECT_EQ(limit_preflight.error().code, "invalid_request");
  EXPECT_EQ(limit_preflight.error().details.at("reason"),
            "gripper_position_limit_violation");
  EXPECT_EQ(channel->publish_calls(), 0);
}

TEST(RetainedGripperCommandBackendTest,
     RejectsNonFiniteStartPositionBeforeScheduling) {
  auto binding = make_binding(CoordinateKind::kLinear);
  auto channel = std::make_shared<FakeRetainedStateChannel>(
      binding, std::vector<double>{std::numeric_limits<double>::quiet_NaN()});
  FakeClock clock;
  ManualScheduler scheduler;
  RetainedGripperCommandBackend backend(
      binding, channel, gripper_limits(binding), clock, scheduler);

  const auto result =
      backend.command_gripper(gripper_request(binding, 0.02, 1.0));

  ASSERT_FALSE(result.accepted);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->code, "gripper_state_not_finite");
  EXPECT_EQ(channel->publish_calls(), 0);
  EXPECT_EQ(backend.get_gripper_status("gripper.target").state,
            GripperCommandState::kIdle);
}

TEST(RetainedGripperCommandBackendTest,
     InvalidationFencesCommandsButKeepsObservedStateFresh) {
  auto binding = make_binding(CoordinateKind::kLinear);
  auto channel = std::make_shared<FakeRetainedStateChannel>(
      binding, std::vector<double>{0.0});
  FakeClock clock;
  ManualScheduler scheduler;
  RetainedGripperCommandBackend backend(
      binding, channel, gripper_limits(binding), clock, scheduler);
  const auto first =
      backend.command_gripper(gripper_request(binding, 0.02, 1.0));
  ASSERT_TRUE(first.accepted);

  backend.invalidate_gripper_commands(GripperInvalidationReason::kStop);
  clock.advance(1'000'000'000);
  scheduler.advance_to(clock.now());
  const auto reset = backend.get_gripper_status("gripper.target");
  EXPECT_EQ(reset.state, GripperCommandState::kIdle);
  EXPECT_NE(reset.generation, first.generation);
  EXPECT_EQ(channel->publish_calls(), 1);
  const auto observed_while_fenced = channel->current_state(clock.now());
  ASSERT_TRUE(observed_while_fenced.ok());
  EXPECT_EQ(observed_while_fenced.value().received_timestamp_ns, clock.now());
  const auto fenced_preflight =
      backend.validate_gripper_command(gripper_request(binding, 0.01, 0.0));
  ASSERT_FALSE(fenced_preflight.ok());
  EXPECT_EQ(fenced_preflight.error().code, "gripper_command_not_authorized");
  EXPECT_FALSE(
      backend.command_gripper(gripper_request(binding, 0.01, 0.0)).accepted);

  backend.authorize_gripper_commands();
  const auto replacement =
      backend.command_gripper(gripper_request(binding, 0.01, 0.0));
  ASSERT_TRUE(replacement.accepted);
  const auto busy_preflight =
      backend.validate_gripper_command(gripper_request(binding, 0.015, 0.0));
  ASSERT_FALSE(busy_preflight.ok());
  EXPECT_EQ(busy_preflight.error().code, "gripper_busy");
  scheduler.advance_to(clock.now());
  EXPECT_NE(replacement.generation, first.generation);
}

TEST(RetainedGripperCommandBackendTest,
     AuthorizationMaintainsFreshIdleStateUntilACommandTakesOwnership) {
  auto binding = make_binding(CoordinateKind::kLinear);
  auto channel = std::make_shared<FakeRetainedStateChannel>(
      binding, std::vector<double>{0.0});
  FakeClock clock;
  ManualScheduler scheduler;
  RetainedGripperCommandBackend backend(
      binding, channel, gripper_limits(binding), clock, scheduler);
  const auto first =
      backend.command_gripper(gripper_request(binding, 0.02, 1.0));
  ASSERT_TRUE(first.accepted);

  backend.invalidate_gripper_commands(GripperInvalidationReason::kReset);
  clock.advance(25'000'000);
  backend.authorize_gripper_commands();
  ASSERT_EQ(channel->publish_calls(), 1);
  EXPECT_EQ(backend.get_gripper_status("gripper.target").state,
            GripperCommandState::kIdle);

  clock.advance(10'000'000);
  scheduler.advance_to(clock.now());
  ASSERT_EQ(channel->publish_calls(), 2);
  auto observed = channel->current_state(clock.now());
  ASSERT_TRUE(observed.ok());
  EXPECT_EQ(observed.value().received_timestamp_ns, clock.now());

  clock.advance(10'000'000);
  scheduler.advance_to(clock.now());
  ASSERT_EQ(channel->publish_calls(), 3);

  const auto replacement =
      backend.command_gripper(gripper_request(binding, 0.01, 1.0));
  ASSERT_TRUE(replacement.accepted);
  clock.advance(10'000'000);
  scheduler.advance_to(clock.now());
  EXPECT_EQ(channel->publish_calls(), 4);
  EXPECT_EQ(backend.get_gripper_status("gripper.target").state,
            GripperCommandState::kExecuting);
}

TEST(RetainedGripperCommandBackendTest,
     TerminalProjectionRetryIsBoundedAndVisible) {
  auto binding = make_binding(CoordinateKind::kLinear);
  auto channel = std::make_shared<FakeRetainedStateChannel>(
      binding, std::vector<double>{0.0});
  channel->make_projection_busy(10);
  FakeClock clock;
  ManualScheduler scheduler;
  RetainedGripperCommandBackend backend(
      binding, channel, gripper_limits(binding), clock, scheduler, 1);
  ASSERT_TRUE(
      backend.command_gripper(gripper_request(binding, 0.02, 0.0)).accepted);

  for (int attempt = 0; attempt < 12; ++attempt) {
    clock.advance(1);
    scheduler.advance_to(clock.now());
  }

  const auto failed = backend.get_gripper_status("gripper.target");
  EXPECT_EQ(failed.state, GripperCommandState::kFailed);
  EXPECT_EQ(failed.projection_state, GripperProjectionState::kFailed);
  ASSERT_TRUE(failed.error.has_value());
  EXPECT_EQ(failed.error->code, "gripper_projection_failed");
  EXPECT_EQ(channel->project_calls(), 10);
  const auto failed_preflight =
      backend.validate_gripper_command(gripper_request(binding, 0.01, 0.0));
  ASSERT_FALSE(failed_preflight.ok());
  EXPECT_EQ(failed_preflight.error().code, "gripper_projection_failed");
}

TEST(RetainedGripperCommandBackendTest,
     PreflightInvalidationAndStaleGraspLossCannotResurrectOldAuthority) {
  auto binding = make_binding(CoordinateKind::kLinear);
  auto channel = std::make_shared<FakeRetainedStateChannel>(
      binding, std::vector<double>{0.0});
  channel->block_read();
  FakeClock clock;
  ManualScheduler scheduler;
  RetainedGripperCommandBackend backend(
      binding, channel, gripper_limits(binding), clock, scheduler);
  auto pending = std::async(std::launch::async, [&] {
    return backend.command_gripper(gripper_request(binding, 0.02, 1.0));
  });
  ASSERT_TRUE(channel->wait_for_read());
  backend.invalidate_gripper_commands(GripperInvalidationReason::kReset);
  channel->release_read();
  const auto rejected = pending.get();
  ASSERT_FALSE(rejected.accepted);
  ASSERT_TRUE(rejected.error.has_value());
  EXPECT_EQ(rejected.error->code, "gripper_command_not_authorized");

  backend.authorize_gripper_commands();
  const auto replacement =
      backend.command_gripper(gripper_request(binding, 0.01, 1.0));
  ASSERT_TRUE(replacement.accepted);
  const auto stale = backend.mark_gripper_grasp_lost(
      "gripper.target", rejected.command_id, rejected.generation,
      "pressure_switch", "delayed loss", "grasp_confirmation_lost");
  EXPECT_FALSE(stale.accepted);
  EXPECT_EQ(backend.get_gripper_status("gripper.target").generation,
            replacement.generation);
}

TEST(RetainedGripperCommandBackendTest,
     InvalidationLinearizesAgainstDequeuedStatePublication) {
  auto binding = make_binding(CoordinateKind::kLinear);
  auto channel = std::make_shared<FakeRetainedStateChannel>(
      binding, std::vector<double>{0.0});
  channel->block_publish();
  FakeClock clock;
  ManualScheduler scheduler;
  RetainedGripperCommandBackend backend(
      binding, channel, gripper_limits(binding), clock, scheduler);

  const auto admitted =
      backend.command_gripper(gripper_request(binding, 0.02, 0.0));
  ASSERT_TRUE(admitted.accepted);
  auto callback = std::async(std::launch::async,
                             [&] { scheduler.advance_to(clock.now()); });
  ASSERT_TRUE(channel->wait_for_publish());

  auto invalidate = std::async(std::launch::async, [&] {
    backend.invalidate_gripper_commands(GripperInvalidationReason::kReset);
  });
  EXPECT_EQ(invalidate.wait_for(20ms), std::future_status::timeout);

  channel->release_publish();
  callback.get();
  invalidate.get();
  const auto calls_after_invalidation = channel->publish_calls();
  clock.advance(1'000'000'000);
  scheduler.advance_to(clock.now());
  EXPECT_EQ(channel->publish_calls(), calls_after_invalidation + 1);
  EXPECT_EQ(backend.get_gripper_status("gripper.target").state,
            GripperCommandState::kIdle);
}

TEST(RetainedGripperCommandBackendTest,
     GraspLossAdvancesGenerationAndPreservesEvidence) {
  auto binding = make_binding(CoordinateKind::kLinear);
  auto channel = std::make_shared<FakeRetainedStateChannel>(
      binding, std::vector<double>{0.0});
  FakeClock clock;
  ManualScheduler scheduler;
  RetainedGripperCommandBackend backend(
      binding, channel, gripper_limits(binding), clock, scheduler);
  const auto admitted =
      backend.command_gripper(gripper_request(binding, 0.02, 1.0));
  ASSERT_TRUE(admitted.accepted);

  const auto lost = backend.mark_gripper_grasp_lost(
      "gripper.target", admitted.command_id, admitted.generation,
      "pressure_switch", "pressure lost", "grasp_confirmation_lost");

  ASSERT_TRUE(lost.accepted);
  EXPECT_NE(lost.generation, admitted.generation);
  const auto status = backend.get_gripper_status("gripper.target");
  EXPECT_EQ(status.state, GripperCommandState::kFailed);
  EXPECT_EQ(status.evidence_state, "pressure_or_part_lost");
  ASSERT_FALSE(status.evidence.empty());
  EXPECT_EQ(status.evidence.back(), "pressure_switch");
}

TEST(RetainedGripperCommandBackendTest,
     SchedulerFailureAndInlineSideEffectsHaveTruthfulAdmission) {
  auto binding = make_binding(CoordinateKind::kLinear);
  FakeClock clock;
  {
    auto channel = std::make_shared<FakeRetainedStateChannel>(
        binding, std::vector<double>{0.0});
    RejectingScheduler scheduler;
    RetainedGripperCommandBackend backend(
        binding, channel, gripper_limits(binding), clock, scheduler);
    const auto result =
        backend.command_gripper(gripper_request(binding, 0.02, 1.0));
    ASSERT_FALSE(result.accepted);
    ASSERT_TRUE(result.error.has_value());
    EXPECT_EQ(result.error->code, "gripper_request_failed");
  }
  {
    auto channel = std::make_shared<FakeRetainedStateChannel>(
        binding, std::vector<double>{0.0});
    channel->make_projection_busy(1);
    InlineScheduler scheduler;
    RetainedGripperCommandBackend backend(
        binding, channel, gripper_limits(binding), clock, scheduler, 1);
    const auto result =
        backend.command_gripper(gripper_request(binding, 0.02, 0.0));
    EXPECT_TRUE(result.accepted);
    EXPECT_EQ(backend.get_gripper_status("gripper.target").state,
              GripperCommandState::kCompleted);
  }
  {
    auto channel = std::make_shared<FakeRetainedStateChannel>(
        binding, std::vector<double>{0.0});
    InlineThenThrowScheduler scheduler;
    RetainedGripperCommandBackend backend(
        binding, channel, gripper_limits(binding), clock, scheduler);
    const auto result =
        backend.command_gripper(gripper_request(binding, 0.02, 0.0));
    EXPECT_TRUE(result.accepted);
    EXPECT_EQ(backend.get_gripper_status("gripper.target").state,
              GripperCommandState::kCompleted);
  }
}

TEST(RoboPlanExecutionBackendTest,
     RejectsCrossedStateChannelAndLinearTrajectoryTarget) {
  auto motion = make_binding(CoordinateKind::kAngular, "arm.target");
  auto other = make_binding(CoordinateKind::kAngular, "other.target");
  auto crossed = std::make_shared<FakeRetainedStateChannel>(
      other, std::vector<double>{0.0, 0.0});
  AutoAdvanceClock clock;
  EXPECT_THROW(RoboPlanExecutionBackend(motion, crossed, clock),
               std::invalid_argument);

  auto linear = make_binding(CoordinateKind::kLinear);
  auto linear_channel = std::make_shared<FakeRetainedStateChannel>(
      linear, std::vector<double>{0.0});
  EXPECT_THROW(RoboPlanExecutionBackend(linear, linear_channel, clock),
               std::invalid_argument);
}

TEST(RoboPlanExecutionBackendTest,
     ExecutesExactTrajectoryAndPublishesAuthoritativeTerminalState) {
  auto binding = make_binding(CoordinateKind::kAngular, "arm.target");
  auto channel = std::make_shared<FakeRetainedStateChannel>(
      binding, std::vector<double>{0.0, 0.0});
  AutoAdvanceClock clock;
  RoboPlanExecutionBackend backend(binding, channel, clock);

  const auto handle = backend.execute_trajectory(trajectory(binding));

  ASSERT_NE(handle.execution_generation, 0U);
  ASSERT_TRUE(handle.wait_for_completion(2'000'000'000));
  const auto snapshot = handle.get_snapshot();
  EXPECT_EQ(snapshot.state, ExecutionState::kCompleted);
  EXPECT_EQ(snapshot.joint_positions_rad, (std::vector<double>{0.2, -0.3}));
  const auto state = channel->current_controlled_state();
  ASSERT_TRUE(state.ok());
  EXPECT_DOUBLE_EQ(
      std::get<AngularPositionDTO>(state.value().positions[0]).value_rad, 0.2);
  EXPECT_DOUBLE_EQ(
      std::get<AngularPositionDTO>(state.value().positions[1]).value_rad, -0.3);
  EXPECT_TRUE(backend.sync_observed_state_before_planning().ok());
}

TEST(RoboPlanExecutionBackendTest,
     MaintainsFreshOwnerIssuedMotionStateWhileIdle) {
  auto binding = make_binding(CoordinateKind::kAngular, "arm.target");
  auto channel = std::make_shared<FakeRetainedStateChannel>(
      binding, std::vector<double>{0.1, -0.2});
  FakeClock clock;
  ManualScheduler scheduler;
  RoboPlanExecutionBackend backend(binding, channel, clock, scheduler);

  const int initial_publications = channel->publish_calls();
  ASSERT_GT(initial_publications, 0);
  EXPECT_EQ(scheduler.last_priority(), SchedulerPriority::RuntimeSafety);

  clock.advance(10'000'000);
  scheduler.advance_to(clock.now());
  EXPECT_EQ(channel->publish_calls(), initial_publications + 1);
  const auto observed = channel->current_state(clock.now());
  ASSERT_TRUE(observed.ok());
  EXPECT_EQ(observed.value().received_timestamp_ns, clock.now());
  ASSERT_TRUE(observed.value().velocities.has_value());
  ASSERT_EQ(observed.value().velocities->size(), 2U);
  EXPECT_DOUBLE_EQ(
      std::get<AngularVelocityDTO>((*observed.value().velocities)[0])
          .value_rad_s,
      0.0);
  EXPECT_DOUBLE_EQ(
      std::get<AngularVelocityDTO>((*observed.value().velocities)[1])
          .value_rad_s,
      0.0);
}

TEST(RoboPlanExecutionBackendTest,
     PauseResumeResetRemainBoundedThroughTheAddressedStateChannel) {
  auto binding = make_binding(CoordinateKind::kAngular, "arm.target");
  auto channel = std::make_shared<FakeRetainedStateChannel>(
      binding, std::vector<double>{0.0, 0.0});
  FakeClock clock;
  RoboPlanExecutionBackend backend(binding, channel, clock);
  auto active = backend.execute_trajectory(trajectory(binding, 1.0));
  ASSERT_TRUE(backend.pause().accepted);

  const auto pause_deadline = std::chrono::steady_clock::now() + 1s;
  while (backend.get_execution_snapshot().state != ExecutionState::kPaused &&
         std::chrono::steady_clock::now() < pause_deadline) {
    std::this_thread::yield();
  }
  EXPECT_EQ(backend.get_execution_snapshot().state, ExecutionState::kPaused);

  auto resumed = backend.resume(trajectory(binding, 0.05));
  const auto completion_deadline = std::chrono::steady_clock::now() + 2s;
  while (resumed.get_snapshot().state != ExecutionState::kCompleted &&
         std::chrono::steady_clock::now() < completion_deadline) {
    clock.advance(10'000'000);
    std::this_thread::yield();
  }
  ASSERT_TRUE(resumed.wait_for_completion(2'000'000'000));
  EXPECT_EQ(resumed.get_snapshot().state, ExecutionState::kCompleted);
  EXPECT_TRUE(backend.reset().accepted);
  EXPECT_EQ(backend.get_execution_snapshot().state, ExecutionState::kIdle);
  (void)active;
}

TEST(RoboPlanExecutionBackendTest,
     ProjectionContentionDoesNotDiscardOwnerIssuedState) {
  auto binding = make_binding(CoordinateKind::kAngular, "arm.target");
  auto channel = std::make_shared<FakeRetainedStateChannel>(
      binding, std::vector<double>{0.0, 0.0});
  channel->make_projection_busy(4);
  AutoAdvanceClock clock;
  RoboPlanExecutionBackend backend(binding, channel, clock);

  auto handle = backend.execute_trajectory(trajectory(binding));
  ASSERT_TRUE(handle.wait_for_completion(2'000'000'000));
  const auto state = channel->current_controlled_state();
  ASSERT_TRUE(state.ok());
  EXPECT_DOUBLE_EQ(
      std::get<AngularPositionDTO>(state.value().positions[0]).value_rad, 0.2);
  EXPECT_GT(channel->publish_calls(), 0);
  EXPECT_GT(backend.scene_projection_metrics().misses_total, 0U);
}

class GripperRuntimeOwnerTestBackend final : public IGripperCommandBackend {
 public:
  explicit GripperRuntimeOwnerTestBackend(std::string gripper_id) {
    status.gripper_id = std::move(gripper_id);
  }

  GripperCommandResultDTO command_gripper(const GripperCommandRequestDTO&) override { return {}; }

  GripperCommandStatusDTO get_gripper_status(const std::string&) const override {
    std::lock_guard lock(mutex);
    return status;
  }

  GripperCommandResultDTO mark_gripper_grasp_lost(const std::string&, std::uint64_t, std::uint64_t,
                                                  const std::string&, const std::string&,
                                                  const std::string&) override {
    return {};
  }

  void invalidate_gripper_commands(GripperInvalidationReason) noexcept override {
    std::lock_guard lock(mutex);
    status.state = GripperCommandState::kIdle;
  }

  void authorize_gripper_commands() noexcept override {}

  void set_state(const GripperCommandState state) {
    std::lock_guard lock(mutex);
    status.state = state;
  }

  void set_gripper_id(std::string gripper_id) {
    std::lock_guard lock(mutex);
    status.gripper_id = std::move(gripper_id);
  }

 private:
  mutable std::mutex mutex;
  GripperCommandStatusDTO status;
};

[[nodiscard]] RuntimeParticipantBinding make_next_publication_gripper_binding(
    const RuntimeParticipantBinding& predecessor) {
  auto candidate = predecessor.value();
  candidate.program_revision = ProgramRevision("program-revision.next");
  candidate.program_binding_id = ProgramBindingId("program-binding.next");
  candidate.retained_bundle_id = RetainedBundleId("bundle.next");
  candidate.graph_revision = GraphRevision("graph.next");
  auto sealed = verify_runtime_participant_binding(std::move(candidate));
  if (!sealed.ok()) {
    throw std::logic_error(sealed.error().message);
  }
  return std::move(sealed.value());
}

[[nodiscard]] RuntimeParticipantBinding make_restarted_gripper_binding(
    const RuntimeParticipantBinding& predecessor) {
  auto candidate = predecessor.value();
  candidate.control_path->control_route_id = ControlRouteId("route.successor");
  candidate.control_path->active_control_configuration_id =
      ActiveControlConfigurationId("active.native.successor");
  candidate.control_path->active_control_configuration_generation =
      ActiveControlConfigurationGeneration(2U);
  candidate.control_path->provider_runtime_epoch = RuntimeEpoch("runtime-epoch.2");
  for (auto& runtime : candidate.runtime_unit_epochs) {
    runtime.runtime_epoch = RuntimeEpoch("runtime-epoch.2");
  }
  auto sealed = verify_runtime_participant_binding(std::move(candidate));
  if (!sealed.ok()) {
    throw std::logic_error(sealed.error().message);
  }
  return std::move(sealed.value());
}

[[nodiscard]] ActiveControlConfigurationDTO gripper_route_configuration(
    const RuntimeParticipantBinding& binding) {
  const auto& path = *binding.value().control_path;
  return ActiveControlConfigurationDTO{
      path.active_control_configuration_id,
      path.control_domain_id,
      path.control_domain_revision,
      {{path.control_binding_id, path.control_binding_revision,
        path.control_route_id}},
      path.activation_set_id,
      path.controller_topology_id,
      path.topology_revision,
      path.provider_runtime_unit_id,
      path.provider_runtime_epoch,
      1U,
      {},
      path.active_control_configuration_generation,
      ActiveControlConfigurationStateDTO::kReconciled};
}

[[nodiscard]] std::shared_ptr<RetainedObservedStateChannel>
make_gripper_restart_state_channel(const RuntimeParticipantBinding& binding,
                                   IClock& clock) {
  auto channel = std::make_shared<RetainedObservedStateChannel>(
      binding, nullptr,
      RetainedObservedStateChannelConfig{
          ObservedStateSourceKind::kPredictedState,
          "native.gripper.restart",
          1'000'000'000,
          1U},
      &clock);
  const std::array<double, 1U> position{0.0};
  const std::array<double, 1U> velocity{0.0};
  if (!channel->publish_state(position, velocity, clock.now(), clock.now())
           .applied()) {
    throw std::logic_error("could not seed the restarted gripper state channel");
  }
  return channel;
}

TEST(RetainedNativeGripperRuntimeTest,
     RegistrationOwnsOnlyTheGripperEngineAndRouteRebindPreservesIt) {
  auto binding = make_binding(CoordinateKind::kLinear);
  auto backend =
      std::make_shared<GripperRuntimeOwnerTestBackend>(binding.value().command_target_id.value());
  auto clock = std::make_shared<FakeClock>();
  auto state_channel =
      std::make_shared<FakeRetainedStateChannel>(binding, std::vector<double>{0.0});
  int rebind_calls = 0;
  std::optional<RuntimeParticipantBinding> rebound_binding;
  std::shared_ptr<IRetainedObservedStateChannel> rebound_channel;
  RetainedNativeGripperRuntime owner(RetainedNativeGripperRuntimeInput{
      binding, backend, clock, state_channel, nullptr, std::nullopt, true,
      [&](const RuntimeParticipantBinding& candidate,
          const std::shared_ptr<IRetainedObservedStateChannel>& candidate_channel,
          const std::optional<RuntimeParticipantStopReadiness>& readiness) {
        ++rebind_calls;
        EXPECT_FALSE(readiness.has_value());
        rebound_binding = candidate;
        rebound_channel = candidate_channel;
        return Result<void>::Ok();
      }, {}, std::nullopt});

  const auto before = owner.registration();
  ASSERT_NE(before.engine, nullptr);
  EXPECT_NE(dynamic_cast<GripperParticipantEngine*>(before.engine), nullptr);
  EXPECT_EQ(before.stop_execution_port, nullptr);
  EXPECT_FALSE(before.stop_readiness.has_value());
  EXPECT_FALSE(before.participant_policy.has_value());
  EXPECT_TRUE(before.intrinsic_direct_control);
  EXPECT_EQ(owner.backend(), backend.get());
  EXPECT_EQ(owner.state_channel(), state_channel);

  const auto candidate = make_next_publication_gripper_binding(binding);
  auto candidate_channel =
      std::make_shared<FakeRetainedStateChannel>(candidate, std::vector<double>{0.0});
  const auto other_binding = make_binding(CoordinateKind::kLinear, "another.gripper");
  auto mismatched_channel =
      std::make_shared<FakeRetainedStateChannel>(other_binding, std::vector<double>{0.0});
  const auto mismatched = owner.RebindRoute(candidate, mismatched_channel, std::nullopt);
  ASSERT_FALSE(mismatched.ok());
  EXPECT_EQ(mismatched.error().code, "native_gripper_state_channel_binding_mismatch");
  EXPECT_EQ(rebind_calls, 0);
  EXPECT_EQ(owner.binding().value(), binding.value());
  EXPECT_EQ(owner.state_channel(), state_channel);

  ASSERT_TRUE(owner.RebindRoute(candidate, candidate_channel, std::nullopt).ok());
  const auto after = owner.registration();
  EXPECT_EQ(rebind_calls, 1);
  ASSERT_TRUE(rebound_binding.has_value());
  EXPECT_EQ(rebound_binding->value(), candidate.value());
  EXPECT_EQ(rebound_channel, candidate_channel);
  EXPECT_EQ(after.binding.value(), candidate.value());
  EXPECT_EQ(owner.state_channel(), candidate_channel);
  EXPECT_EQ(after.engine, before.engine);
  EXPECT_EQ(owner.participant_engine(), before.engine);
  EXPECT_EQ(owner.backend(), backend.get());
}

TEST(RetainedNativeGripperRuntimeTest, FailedOrUnverifiedRebindKeepsThePredecessorRegistration) {
  auto binding = make_binding(CoordinateKind::kLinear);
  auto backend =
      std::make_shared<GripperRuntimeOwnerTestBackend>(binding.value().command_target_id.value());
  auto clock = std::make_shared<FakeClock>();
  auto state_channel =
      std::make_shared<FakeRetainedStateChannel>(binding, std::vector<double>{0.0});
  RetainedNativeGripperRuntime owner(RetainedNativeGripperRuntimeInput{
      binding, backend, clock, state_channel, nullptr, std::nullopt, false,
      [](const RuntimeParticipantBinding&, const std::shared_ptr<IRetainedObservedStateChannel>&,
         const std::optional<RuntimeParticipantStopReadiness>&) {
        return Result<void>::Err(ErrorDTO{
            "native_route_readback_mismatch", "candidate gripper route did not verify", {}});
      }, {}, std::nullopt});
  const auto before = owner.registration();
  const auto candidate = make_next_publication_gripper_binding(binding);
  auto candidate_channel =
      std::make_shared<FakeRetainedStateChannel>(candidate, std::vector<double>{0.0});

  const auto refused = owner.RebindRoute(candidate, candidate_channel, std::nullopt);
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().code, "native_route_readback_mismatch");
  EXPECT_EQ(owner.registration().binding.value(), before.binding.value());
  EXPECT_EQ(owner.registration().engine, before.engine);
  EXPECT_EQ(owner.state_channel(), state_channel);

  RetainedNativeGripperRuntime no_rebinder(RetainedNativeGripperRuntimeInput{
      binding, backend, clock, state_channel, nullptr, std::nullopt, false, {},
      {}, std::nullopt});
  const auto unverified = no_rebinder.RebindRoute(candidate, candidate_channel, std::nullopt);
  ASSERT_FALSE(unverified.ok());
  EXPECT_EQ(unverified.error().code, "native_gripper_route_rebind_unverified");
  EXPECT_EQ(no_rebinder.registration().binding.value(), binding.value());
  EXPECT_EQ(no_rebinder.state_channel(), state_channel);
}

TEST(RetainedNativeGripperRuntimeTest, ConstructionRequiresAnExactObservedStateChannel) {
  auto binding = make_binding(CoordinateKind::kLinear);
  auto other_binding = make_binding(CoordinateKind::kLinear, "other.gripper");
  auto backend =
      std::make_shared<GripperRuntimeOwnerTestBackend>(binding.value().command_target_id.value());
  auto clock = std::make_shared<FakeClock>();
  auto mismatched_channel =
      std::make_shared<FakeRetainedStateChannel>(other_binding, std::vector<double>{0.0});

  EXPECT_THROW(
      {
        RetainedNativeGripperRuntime owner(RetainedNativeGripperRuntimeInput{
            binding, backend, clock, mismatched_channel, nullptr, std::nullopt,
            false, {}, {}, std::nullopt});
      },
      std::invalid_argument);
  EXPECT_THROW(
      {
        RetainedNativeGripperRuntime owner(RetainedNativeGripperRuntimeInput{
            binding, backend, clock, {}, nullptr, std::nullopt, false, {},
            {}, std::nullopt});
      },
      std::invalid_argument);
}

TEST(RetainedNativeGripperRuntimeTest, RouteRebindRefusesAnActiveOrMismatchedGripperStatus) {
  auto binding = make_binding(CoordinateKind::kLinear);
  auto backend =
      std::make_shared<GripperRuntimeOwnerTestBackend>(binding.value().command_target_id.value());
  auto clock = std::make_shared<FakeClock>();
  auto state_channel =
      std::make_shared<FakeRetainedStateChannel>(binding, std::vector<double>{0.0});
  int rebind_calls = 0;
  RetainedNativeGripperRuntime owner(RetainedNativeGripperRuntimeInput{
      binding, backend, clock, state_channel, nullptr, std::nullopt, false,
      [&](const RuntimeParticipantBinding&, const std::shared_ptr<IRetainedObservedStateChannel>&,
          const std::optional<RuntimeParticipantStopReadiness>&) {
        ++rebind_calls;
        return Result<void>::Ok();
      }, {}, std::nullopt});
  const auto candidate = make_next_publication_gripper_binding(binding);
  auto candidate_channel =
      std::make_shared<FakeRetainedStateChannel>(candidate, std::vector<double>{0.0});

  backend->set_state(GripperCommandState::kExecuting);
  const auto active = owner.RebindRoute(candidate, candidate_channel, std::nullopt);
  ASSERT_FALSE(active.ok());
  EXPECT_EQ(active.error().code, "native_gripper_route_rebind_busy");
  EXPECT_EQ(rebind_calls, 0);
  EXPECT_EQ(owner.registration().binding.value(), binding.value());

  backend->set_state(GripperCommandState::kIdle);
  backend->set_gripper_id("another.gripper");
  const auto mismatched = owner.RebindRoute(candidate, candidate_channel, std::nullopt);
  ASSERT_FALSE(mismatched.ok());
  EXPECT_EQ(mismatched.error().code, "native_gripper_route_rebind_busy");
  EXPECT_EQ(rebind_calls, 0);

  backend->set_gripper_id(binding.value().command_target_id.value());
  ASSERT_TRUE(owner.RebindRoute(candidate, candidate_channel, std::nullopt).ok());
  EXPECT_EQ(rebind_calls, 1);
  EXPECT_EQ(owner.state_channel(), candidate_channel);
}

TEST(RetainedNativeGripperRuntimeTest, NativeFactoryOwnsGripperBackendSchedulerAndStopPort) {
  auto binding = make_binding(CoordinateKind::kLinear, "gripper.target", true);
  auto state_channel =
      std::make_shared<FakeRetainedStateChannel>(binding, std::vector<double>{0.0});
  auto clock = std::make_shared<FakeClock>();
  auto scheduler = std::make_shared<ManualScheduler>();

  auto owner = RetainedNativeGripperRuntime::CreateNative(binding, state_channel, clock, scheduler,
                                                          std::nullopt, std::nullopt);

  ASSERT_NE(owner, nullptr);
  auto* backend = dynamic_cast<RetainedGripperCommandBackend*>(owner->backend());
  ASSERT_NE(backend, nullptr);
  ASSERT_NE(owner->participant_engine(), nullptr);
  EXPECT_NE(dynamic_cast<GripperParticipantEngine*>(owner->registration().engine), nullptr);
  EXPECT_EQ(owner->state_channel(), state_channel);
  EXPECT_EQ(owner->stop_execution_port(), static_cast<IControlDomainExecutionPort*>(backend));
  EXPECT_EQ(owner->registration().stop_execution_port, owner->stop_execution_port());

  const auto candidate = make_next_publication_gripper_binding(binding);
  auto candidate_channel =
      std::make_shared<FakeRetainedStateChannel>(candidate, std::vector<double>{0.0});
  ASSERT_TRUE(owner->RebindRoute(candidate, candidate_channel, std::nullopt).ok());
  EXPECT_EQ(owner->backend(), backend);
  EXPECT_EQ(owner->binding().value(), candidate.value());
  EXPECT_EQ(owner->state_channel(), candidate_channel);
  EXPECT_EQ(owner->stop_execution_port(), static_cast<IControlDomainExecutionPort*>(backend));

  auto shared_owner = std::shared_ptr<RetainedNativeGripperRuntime>(std::move(owner));
  RetainedNativeComponentOwner component_owner(shared_owner);
  EXPECT_EQ(component_owner.motion_runtime(), nullptr);
  EXPECT_EQ(component_owner.gripper_runtime(), shared_owner.get());
  EXPECT_EQ(component_owner.component_id(), binding.value().controllable_component_id);
  const auto physical_bindings = component_owner.physical_bindings();
  ASSERT_EQ(physical_bindings.size(), 1U);
  EXPECT_EQ(physical_bindings.front().value(), candidate.value());
  const auto registrations = component_owner.registrations();
  ASSERT_EQ(registrations.size(), 1U);
  EXPECT_EQ(registrations.front().binding.value(), candidate.value());
  EXPECT_EQ(registrations.front().engine, shared_owner->participant_engine());
}

TEST(RetainedNativeGripperRoutePublicationOwnerTest,
     PublishesClosedUnreconciledFactsAndRequiresCompleteCellBuilderForReconcile) {
  using namespace core;

  auto binding = make_binding(CoordinateKind::kLinear, "gripper.target", true);
  auto backend = std::make_shared<GripperRuntimeOwnerTestBackend>(
      binding.value().command_target_id.value());
  auto clock = std::make_shared<FakeClock>();
  auto scheduler = std::make_shared<ManualScheduler>();
  auto state_channel = std::make_shared<FakeRetainedStateChannel>(
      binding, std::vector<double>{0.0});
  auto gripper = std::make_shared<RetainedNativeGripperRuntime>(
      RetainedNativeGripperRuntimeInput{binding, backend, clock, state_channel,
                                        nullptr, std::nullopt, false, {},
                                        scheduler, std::nullopt});

  const auto cell_id = CellId("cell.native.gripper.route");
  const auto graph_revision = binding.value().graph_revision;
  const auto journal_path = std::filesystem::temp_directory_path() /
                            ("manyforge-gripper-route-" +
                             std::to_string(
                                 std::chrono::steady_clock::now()
                                     .time_since_epoch()
                                     .count()) +
                             ".sqlite");
  {
    SqliteOperationJournal journal(journal_path, cell_id, 16U);
    ResourceAddressedExecutionKernel kernel(
        {gripper->registration()}, *clock, *scheduler, journal,
        ResourceAddressedExecutionKernelConfig{
            RequestOwnerEpoch("owner.native.gripper.route"), cell_id, 16U,
            {}, nullptr, graph_revision});
    RetainedNativeGripperRoutePublicationOwner owner(gripper, *clock, kernel);

    const auto& path = *binding.value().control_path;
    ActiveControlConfigurationDTO reconciled{
        ActiveControlConfigurationId("active.native.gripper.reconciled"),
        path.control_domain_id,
        path.control_domain_revision,
        {{path.control_binding_id, path.control_binding_revision,
          path.control_route_id}},
        ActivationSetId("activation.native.gripper"),
        path.controller_topology_id,
        path.topology_revision,
        RuntimeUnitId("runtime.native.gripper"),
        RuntimeEpoch("runtime.native.gripper.1"),
        1U,
        {},
        ActiveControlConfigurationGeneration(1U),
        ActiveControlConfigurationStateDTO::kReconciled};
    const auto current = owner.ReadCurrentPhysicalParticipants(reconciled);
    ASSERT_TRUE(current.ok()) << current.error().message;
    ASSERT_EQ(current.value().size(), 1U);
    EXPECT_EQ(current.value().front().value(), binding.value());

    auto unreconciled = reconciled;
    unreconciled.active_control_configuration_id =
        ActiveControlConfigurationId("active.native.gripper.external");
    unreconciled.active_control_configuration_generation =
        ActiveControlConfigurationGeneration(2U);
    unreconciled.binding_routes.front().control_route_id =
        ControlRouteId("route.external.gripper");
    unreconciled.state =
        ActiveControlConfigurationStateDTO::kExternalOrUnknownUnreconciled;

    const auto published = owner.PublishAndReadBackRouteConfiguration(
        unreconciled, {});
    ASSERT_TRUE(published.ok()) << published.error().message;
    EXPECT_EQ(published.value().active_configuration, unreconciled);
    EXPECT_TRUE(published.value().readiness_rows.empty());
    EXPECT_FALSE(published.value().operation_admission_open);

    auto unsupported_reconciled = unreconciled;
    unsupported_reconciled.state =
        ActiveControlConfigurationStateDTO::kReconciled;
    unsupported_reconciled.active_control_configuration_generation =
        ActiveControlConfigurationGeneration(3U);
    const auto refused = owner.PublishAndReadBackRouteConfiguration(
        unsupported_reconciled, {});
    ASSERT_FALSE(refused.ok());
    ASSERT_EQ(refused.error().details.size(), 1U);
    EXPECT_EQ(refused.error().details.at("reason"),
              "route_publication_gripper_cell_builder_missing");

    const auto readback = kernel.runtime_route_configuration_readback(
        unreconciled.control_domain_id,
        unreconciled.active_control_configuration_id,
        unreconciled.active_control_configuration_generation);
    ASSERT_TRUE(readback.ok()) << readback.error().message;
    EXPECT_EQ(readback.value(), published.value());
  }
  std::error_code ignored;
  std::filesystem::remove(journal_path, ignored);
  std::filesystem::remove(journal_path.string() + "-wal", ignored);
  std::filesystem::remove(journal_path.string() + "-shm", ignored);
}

TEST(RetainedNativeGripperRoutePublicationOwnerTest,
     AdoptsDurablePredecessorAfterRuntimeRestartAndKeepsAdmissionClosed) {
  using namespace core;

  const auto durable_binding =
      make_binding(CoordinateKind::kLinear, "gripper.target", true);
  const auto predecessor = gripper_route_configuration(durable_binding);
  const auto current_binding = make_restarted_gripper_binding(durable_binding);
  auto clock = std::make_shared<FakeClock>();
  clock->set(100);
  auto scheduler = std::make_shared<ManualScheduler>();
  auto state_channel = make_gripper_restart_state_channel(current_binding, *clock);
  auto backend = std::make_shared<GripperRuntimeOwnerTestBackend>(
      current_binding.value().command_target_id.value());
  std::vector<ControlRouteId> rebound_routes;
  auto gripper = std::make_shared<RetainedNativeGripperRuntime>(
      RetainedNativeGripperRuntimeInput{
          current_binding,
          backend,
          clock,
          state_channel,
          nullptr,
          std::nullopt,
          false,
          [&](const RuntimeParticipantBinding& candidate,
              const std::shared_ptr<IRetainedObservedStateChannel>& channel,
              const std::optional<RuntimeParticipantStopReadiness>& readiness) {
            EXPECT_EQ(channel, state_channel);
            EXPECT_FALSE(readiness.has_value());
            rebound_routes.push_back(candidate.value().control_path->control_route_id);
            return Result<void>::Ok();
          }, scheduler, std::nullopt});

  const auto cell_id = CellId("cell.native.gripper.restart");
  const auto journal_path = std::filesystem::temp_directory_path() /
                            ("manyforge-gripper-restart-" +
                             std::to_string(std::chrono::steady_clock::now()
                                                .time_since_epoch()
                                                .count()) +
                             ".sqlite");
  {
    SqliteOperationJournal journal(journal_path, cell_id, 16U);
    ResourceAddressedExecutionKernel kernel(
        {gripper->registration()}, *clock, *scheduler, journal,
        ResourceAddressedExecutionKernelConfig{
            RequestOwnerEpoch("owner.native.gripper.restart"), cell_id, 16U,
            {}, nullptr, current_binding.value().graph_revision});
    RetainedNativeGripperRoutePublicationOwner owner(
        gripper, *clock, kernel,
        [](const ActiveControlConfigurationDTO&,
           const RuntimeParticipantExecutionRegistration& registration) {
          RetainedNativeGripperRoutePublicationOwner::KernelCandidate candidate;
          candidate.registrations.push_back(registration);
          return Result<RetainedNativeGripperRoutePublicationOwner::KernelCandidate>::Ok(
              std::move(candidate));
        });

    ASSERT_TRUE(kernel.SetRouteRecoveryPending(true).ok());
    const auto adopted =
        owner.AdoptAndReadBackRouteConfigurationPredecessor(predecessor);
    ASSERT_TRUE(adopted.ok()) << adopted.error().message;
    EXPECT_EQ(adopted.value().active_configuration, predecessor);
    EXPECT_TRUE(adopted.value().readiness_rows.empty());
    EXPECT_FALSE(adopted.value().operation_admission_open);
    EXPECT_EQ(rebound_routes, (std::vector<ControlRouteId>{
                                  predecessor.binding_routes.front().control_route_id}));
    EXPECT_EQ(gripper->binding().value().control_path->provider_runtime_epoch,
              RuntimeEpoch("runtime-epoch.2"));
    EXPECT_EQ(gripper->binding().value().control_path->control_route_id,
              predecessor.binding_routes.front().control_route_id);
    EXPECT_FALSE(gripper->stop_readiness().has_value());

    const auto readback = kernel.runtime_route_configuration_readback(
        predecessor.control_domain_id,
        predecessor.active_control_configuration_id,
        predecessor.active_control_configuration_generation);
    ASSERT_TRUE(readback.ok()) << readback.error().message;
    EXPECT_EQ(readback.value(), adopted.value());
    EXPECT_FALSE(kernel.GetAuthoringAdmission().authoring_allowed);
  }
  std::error_code ignored;
  std::filesystem::remove(journal_path, ignored);
  std::filesystem::remove(journal_path.string() + "-wal", ignored);
  std::filesystem::remove(journal_path.string() + "-shm", ignored);
}

TEST(RetainedNativeGripperRoutePublicationOwnerTest,
     FailedPredecessorRecoveryRollsBackAndPreservesRecoveryExclusion) {
  using namespace core;

  const auto durable_binding =
      make_binding(CoordinateKind::kLinear, "gripper.target", true);
  const auto predecessor = gripper_route_configuration(durable_binding);
  const auto current_binding = make_restarted_gripper_binding(durable_binding);
  auto clock = std::make_shared<FakeClock>();
  clock->set(100);
  auto scheduler = std::make_shared<ManualScheduler>();
  auto state_channel = make_gripper_restart_state_channel(current_binding, *clock);
  auto backend = std::make_shared<GripperRuntimeOwnerTestBackend>(
      current_binding.value().command_target_id.value());
  std::vector<ControlRouteId> rebound_routes;
  int builder_calls = 0;
  auto gripper = std::make_shared<RetainedNativeGripperRuntime>(
      RetainedNativeGripperRuntimeInput{
          current_binding,
          backend,
          clock,
          state_channel,
          nullptr,
          std::nullopt,
          false,
          [&](const RuntimeParticipantBinding& candidate,
              const std::shared_ptr<IRetainedObservedStateChannel>& channel,
              const std::optional<RuntimeParticipantStopReadiness>& readiness) {
            EXPECT_EQ(channel, state_channel);
            EXPECT_FALSE(readiness.has_value());
            rebound_routes.push_back(candidate.value().control_path->control_route_id);
            return Result<void>::Ok();
          }, scheduler, std::nullopt});

  const auto cell_id = CellId("cell.native.gripper.restart.failure");
  const auto journal_path = std::filesystem::temp_directory_path() /
                            ("manyforge-gripper-restart-failure-" +
                             std::to_string(std::chrono::steady_clock::now()
                                                .time_since_epoch()
                                                .count()) +
                             ".sqlite");
  {
    SqliteOperationJournal journal(journal_path, cell_id, 16U);
    ResourceAddressedExecutionKernel kernel(
        {gripper->registration()}, *clock, *scheduler, journal,
        ResourceAddressedExecutionKernelConfig{
            RequestOwnerEpoch("owner.native.gripper.restart.failure"), cell_id,
            16U, {}, nullptr, current_binding.value().graph_revision});
    RetainedNativeGripperRoutePublicationOwner owner(
        gripper, *clock, kernel,
        [&builder_calls](const ActiveControlConfigurationDTO&,
           const RuntimeParticipantExecutionRegistration&) {
          ++builder_calls;
          return Result<RetainedNativeGripperRoutePublicationOwner::KernelCandidate>::Err(
              ErrorDTO{"test_builder_failed", "injected cell candidate failure", {}});
        });

    ASSERT_TRUE(kernel.SetRouteRecoveryPending(true).ok());
    const auto refused =
        owner.AdoptAndReadBackRouteConfigurationPredecessor(predecessor);
    ASSERT_FALSE(refused.ok());
    ASSERT_TRUE(refused.error().details.contains("reason"));
    EXPECT_EQ(refused.error().details.at("reason"),
              "route_predecessor_gripper_builder_failed");
    EXPECT_EQ(rebound_routes,
              (std::vector<ControlRouteId>{
                  predecessor.binding_routes.front().control_route_id,
                  current_binding.value().control_path->control_route_id}));
    EXPECT_EQ(builder_calls, 1);
    EXPECT_EQ(gripper->binding().value(), current_binding.value());
    EXPECT_EQ(gripper->state_channel(), state_channel);
    EXPECT_FALSE(kernel.GetAuthoringAdmission().authoring_allowed);

    auto changed_activation = predecessor;
    changed_activation.activation_set_id = ActivationSetId("activation.foreign");
    const auto refused_activation =
        owner.AdoptAndReadBackRouteConfigurationPredecessor(changed_activation);
    ASSERT_FALSE(refused_activation.ok());
    ASSERT_TRUE(refused_activation.error().details.contains("reason"));
    EXPECT_EQ(refused_activation.error().details.at("reason"),
              "route_predecessor_gripper_rebind_failed");
    EXPECT_EQ(builder_calls, 1);
    EXPECT_EQ(rebound_routes,
              (std::vector<ControlRouteId>{
                  predecessor.binding_routes.front().control_route_id,
                  current_binding.value().control_path->control_route_id,
                  current_binding.value().control_path->control_route_id}));
    EXPECT_EQ(gripper->binding().value(), current_binding.value());
    EXPECT_EQ(state_channel->query_context(),
              make_observed_state_query_context(
                  current_binding,
                  state_channel->query_context().freshness_budget_ns)
                  .value());
    EXPECT_FALSE(kernel.GetAuthoringAdmission().authoring_allowed);
  }
  std::error_code ignored;
  std::filesystem::remove(journal_path, ignored);
  std::filesystem::remove(journal_path.string() + "-wal", ignored);
  std::filesystem::remove(journal_path.string() + "-shm", ignored);
}

TEST(RetainedNativeGripperRuntimeTest,
     OwnedChannelLeavesComponentLocalObservationUnavailableUntilPublished) {
  auto binding = make_binding(CoordinateKind::kLinear, "gripper.local", true);
  auto clock = std::make_shared<FakeClock>();
  clock->set(100);
  auto scheduler = std::make_shared<ManualScheduler>();
  auto wake = std::make_shared<RetainedObservationWake>();

  auto owner = RetainedNativeGripperRuntime::CreateNativeWithOwnedStateChannel(
      binding, nullptr,
      RetainedObservedStateChannelConfig{
          ObservedStateSourceKind::kPredictedState, "native.gripper.local",
          1'000, 1U},
      clock, scheduler, wake, std::nullopt, std::nullopt);

  ASSERT_NE(owner, nullptr);
  auto channel =
      std::dynamic_pointer_cast<RetainedObservedStateChannel>(
          owner->state_channel());
  ASSERT_NE(channel, nullptr);
  auto shared_owner = std::shared_ptr<RetainedNativeGripperRuntime>(
      std::move(owner));
  RetainedNativeComponentOwner component_owner(shared_owner);
  const auto observed_channels = component_owner.observed_channels();
  ASSERT_EQ(observed_channels.size(), 1U);
  EXPECT_EQ(observed_channels.front(), channel);
  EXPECT_EQ(channel->query_context(),
            make_observed_state_query_context(binding, 1'000).value());
  EXPECT_FALSE(channel->current_state(clock->now()).ok());

  const std::array<double, 1U> positions{0.025};
  const std::array<double, 1U> velocities{0.0};
  EXPECT_TRUE(channel
                  ->publish_state(positions, velocities, clock->now(),
                                  clock->now())
                  .applied());
  const auto observed = channel->current_state(clock->now());
  ASSERT_TRUE(observed.ok()) << observed.error().message;
  EXPECT_DOUBLE_EQ(
      std::get<LinearPositionDTO>(observed.value().positions.front()).value_m,
      positions.front());
}

TEST(RetainedNativeGripperRuntimeTest,
     OwnedChannelRejectsModelBackedBindingWithoutProjector) {
  auto binding = make_binding(CoordinateKind::kLinear, "gripper.model", false);
  auto clock = std::make_shared<FakeClock>();
  auto scheduler = std::make_shared<ManualScheduler>();
  auto wake = std::make_shared<RetainedObservationWake>();

  EXPECT_THROW(
      RetainedNativeGripperRuntime::CreateNativeWithOwnedStateChannel(
          binding, nullptr,
          RetainedObservedStateChannelConfig{
              ObservedStateSourceKind::kPredictedState,
              "native.gripper.model", 1'000, 1U},
          clock, scheduler, wake, std::nullopt, std::nullopt),
      std::invalid_argument);
}

} // namespace
} // namespace manyforge::roboplan_backend
