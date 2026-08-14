#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <limits>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>
#include <manyforge_roboplan/retained_execution_state_sink.hpp>
#include <manyforge_roboplan/retained_gripper_command_backend.hpp>
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
namespace {

using namespace manyforge::core;
using namespace std::chrono_literals;

[[nodiscard]] RuntimeParticipantBinding
make_binding(CoordinateKind kind, std::string target = "gripper.target",
             bool component_local = false) {
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
          ControlRouteId("route.native"),
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

  Result<ControlledStateDTO> current_controlled_state() const override {
    std::unique_lock lock(mutex_);
    if (block_read_) {
      read_entered_ = true;
      read_cv_.notify_all();
      read_cv_.wait_for(lock, 2s, [&] { return release_read_; });
    }
    return Result<ControlledStateDTO>::Ok(state_);
  }

  const ObservedStateQueryContextDTO &query_context() const noexcept override {
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
    release_read_ = false;
  }

  [[nodiscard]] bool wait_for_read() const {
    std::unique_lock lock(mutex_);
    return read_cv_.wait_for(lock, 2s, [&] { return read_entered_; });
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
      "gripper", "finger_joint", 0.02,
      ExecutionStateUpdateMode::kBlocking);

  EXPECT_TRUE(result.applied());
  EXPECT_EQ(channel->publish_calls(), 1);
  EXPECT_EQ(channel->project_applied_calls(), 1);
  EXPECT_DOUBLE_EQ(*sink.group_joint_position("gripper", "finger_joint"),
                   0.02);
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
                    "other", {0.1, 0.2},
                    ExecutionStateUpdateMode::kBlocking)
                .status,
            ExecutionStateSinkStatus::kInvalidGroup);
  EXPECT_EQ(sink.apply_group_joint_positions(
                    "arm", {0.1}, ExecutionStateUpdateMode::kBlocking)
                .status,
            ExecutionStateSinkStatus::kInvalidPositions);
  channel->make_projection_busy(1);
  EXPECT_EQ(sink.apply_group_joint_positions(
                    "arm", {0.1, 0.2},
                    ExecutionStateUpdateMode::kBestEffort)
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
       TrajectoryWaypointDTO{duration_s, {0.2, -0.3}, {0.0, 0.0}, {0.0, 0.0}}}};
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
     InvalidationFencesQueuedGenerationUntilExplicitAuthorization) {
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
  clock.advance(1'000'000'000);
  scheduler.advance_to(clock.now());
  const auto reset = backend.get_gripper_status("gripper.target");
  EXPECT_EQ(reset.state, GripperCommandState::kIdle);
  EXPECT_NE(reset.generation, first.generation);
  EXPECT_EQ(channel->publish_calls(), 0);
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
  EXPECT_EQ(channel->publish_calls(), calls_after_invalidation);
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

} // namespace
} // namespace manyforge::roboplan_backend
