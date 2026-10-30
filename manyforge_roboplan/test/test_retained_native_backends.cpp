#include "../src/program_reset_position_observation.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <manyforge_core/resolved_stop_base_plan_codec.hpp>
#include <manyforge_core/types/command_schema.hpp>
#include <manyforge_core/types/manager_liveness.hpp>
#include <manyforge_core/types/stop_control.hpp>
#include <future>
#include <limits>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/resource_addressed_execution_kernel.hpp>
#include <manyforge_core/sqlite_operation_journal.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>
#include <manyforge_roboplan/retained_execution_state_sink.hpp>
#include <manyforge_roboplan/retained_native_component_owner.hpp>
#include <manyforge_roboplan/retained_native_gripper_runtime.hpp>
#include <manyforge_roboplan/retained_native_gripper_route_publication_owner.hpp>
#include <manyforge_roboplan/retained_observed_state_channel.hpp>
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
     MeasuredRouteRebindNeverRestampsAndWaitsForAFreshMeasurement) {
  // CP8 step 1: a Real arm's addressed state is measured. A route change keeps
  // the physical source but no earlier measurement becomes current under it.
  const auto measured = [](const RuntimeParticipantBinding &binding) {
    auto value = binding.value();
    value.control_path->state_authority_class = StateAuthorityClass::kMeasured;
    auto sealed = verify_runtime_participant_binding(std::move(value));
    if (!sealed.ok()) throw std::logic_error(sealed.error().message);
    return std::move(sealed.value());
  };
  const auto binding = measured(make_binding(
      CoordinateKind::kLinear, "gripper.target", true, "route.native"));
  const auto rebound = measured(make_binding(
      CoordinateKind::kLinear, "gripper.target", true, "route.successor"));
  RetainedObservedStateChannel channel(
      binding, nullptr,
      RetainedObservedStateChannelConfig{
          ObservedStateSourceKind::kJointState, "measured.joint_state", 100, 1U});
  const std::array<double, 1U> first{0.01};
  const std::array<double, 1U> second{0.02};
  const std::array<double, 1U> velocity{0.0};
  EXPECT_FALSE(channel.current_state(100).ok());
  ASSERT_TRUE(channel.publish_state(first, velocity, 100, 100).applied());
  ASSERT_TRUE(channel.current_state(100).ok());
  // The same identity is a no-op and stays readable.
  ASSERT_TRUE(channel.RebindRoute(binding, 101).ok());
  EXPECT_TRUE(channel.current_state(101).ok());
  ASSERT_TRUE(channel.RebindRoute(rebound, 102).ok());
  EXPECT_EQ(channel.binding().value(), rebound.value());
  EXPECT_FALSE(channel.current_state(102).ok());
  EXPECT_FALSE(channel.wait_for_fresh_state(0ns));
  ASSERT_TRUE(channel.publish_state(second, velocity, 103, 103).applied());
  const auto current = channel.current_state(103);
  ASSERT_TRUE(current.ok()) << current.error().message;
  EXPECT_EQ(current.value().control_route_id, ControlRouteId("route.successor"));
  EXPECT_EQ(current.value().state_authority_class, StateAuthorityClass::kMeasured);
  EXPECT_DOUBLE_EQ(
      std::get<LinearPositionDTO>(current.value().positions.front()).value_m, 0.02);
  EXPECT_TRUE(channel.wait_for_fresh_state(0ns));
  // Stale beyond the measured budget.
  EXPECT_FALSE(channel.current_state(204).ok());
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

TEST(RetainedObservedStateChannelTest,
     RenderOnlyCoordinatesNeverCreateAuthoritativeState) {
  const auto binding =
      make_binding(CoordinateKind::kLinear, "gripper.target", true);
  RetainedObservedStateChannel channel(
      binding, nullptr,
      RetainedObservedStateChannelConfig{
          ObservedStateSourceKind::kPredictedState, "native.predicted", 0, 1U});
  const auto names = channel.query_context().joint_names;
  const auto authoritative_before = channel.current_state(100);
  ASSERT_FALSE(authoritative_before.ok());
  const bool fresh_before = channel.wait_for_fresh_state(0ns);
  EXPECT_FALSE(channel.current_bound_render_state().ok());
  const auto generation_before = channel.render_change_generation();

  const std::array expected{0.025};
  ASSERT_TRUE(channel.update_render_positions(binding, names, expected).ok());
  const auto rendered = channel.current_bound_render_state();
  ASSERT_TRUE(rendered.ok());
  EXPECT_EQ(rendered.value().binding.value(), binding.value());
  EXPECT_EQ(rendered.value().compiled_joint_names, names);
  EXPECT_EQ(rendered.value().positions_si, std::vector<double>{0.025});
  EXPECT_EQ(rendered.value().render_incarnation, channel.render_incarnation());
  EXPECT_GT(rendered.value().render_generation, generation_before);
  EXPECT_TRUE(channel.wait_for_render_change(generation_before, 0ns));
  const auto authoritative_after = channel.current_state(100);
  ASSERT_FALSE(authoritative_after.ok());
  EXPECT_EQ(authoritative_after.error().details,
            authoritative_before.error().details);
  EXPECT_FALSE(channel.current_state_bounded(100, 101, {}).ok());
  EXPECT_FALSE(channel.current_controlled_state().ok());
  EXPECT_EQ(channel.wait_for_fresh_state(0ns), fresh_before);
}

TEST(RetainedObservedStateChannelTest,
     RenderFeedbackAndInstantFallbackPreserveAuthoritativeSample) {
  const auto binding =
      make_binding(CoordinateKind::kLinear, "gripper.target", true);
  RetainedObservedStateChannel channel(
      binding, nullptr,
      RetainedObservedStateChannelConfig{
          ObservedStateSourceKind::kPredictedState, "native.predicted", 0, 1U});
  const auto names = channel.query_context().joint_names;
  const std::array initial{0.0}, velocity{0.0};
  ASSERT_TRUE(channel.publish_state(initial, velocity, 100, 100).applied());
  const auto owner_before = channel.current_state(100);
  ASSERT_TRUE(owner_before.ok());
  const auto initial_generation = channel.render_change_generation();

  // The caller selects feedback or admission fallback. Both use only the
  // render cache; the fallback is already at the target on the first read.
  const std::array feedback{0.007}, expected{0.025};
  ASSERT_TRUE(channel.update_render_positions(binding, names, feedback).ok());
  const auto measured_display = channel.current_bound_render_state();
  ASSERT_TRUE(measured_display.ok());
  EXPECT_EQ(measured_display.value().positions_si,
            std::vector<double>{0.007});
  ASSERT_TRUE(channel.update_render_positions(binding, names, expected).ok());
  const auto expected_display = channel.current_bound_render_state();
  ASSERT_TRUE(expected_display.ok());
  EXPECT_EQ(expected_display.value().positions_si,
            std::vector<double>{0.025});
  EXPECT_GT(expected_display.value().render_generation,
            measured_display.value().render_generation);
  EXPECT_GT(measured_display.value().render_generation, initial_generation);
  ASSERT_TRUE(channel.update_render_positions(binding, names, expected).ok());
  EXPECT_EQ(channel.render_change_generation(),
            expected_display.value().render_generation);

  const auto owner_after = channel.current_state(100);
  ASSERT_TRUE(owner_after.ok());
  EXPECT_EQ(owner_after.value(), owner_before.value());
}

TEST(RetainedObservedStateChannelTest,
     DisplayInputIsMarkedUntilTheNextAddressedSampleReplacesIt) {
  // M6: display input moves only the display; on a channel that does not
  // follow display input, the next addressed publication takes the render
  // coordinates back.
  const auto binding =
      make_binding(CoordinateKind::kLinear, "gripper.target", true);
  RetainedObservedStateChannel channel(
      binding, nullptr,
      RetainedObservedStateChannelConfig{
          ObservedStateSourceKind::kPredictedState, "native.predicted", 0, 1U});
  const auto names = channel.query_context().joint_names;
  const std::array initial{0.0}, velocity{0.0};
  ASSERT_TRUE(channel.publish_state(initial, velocity, 100, 100).applied());
  ASSERT_TRUE(channel.current_bound_render_state().ok());
  EXPECT_FALSE(channel.current_bound_render_state().value().display_only);
  const auto owner_before = channel.current_state(100);
  ASSERT_TRUE(owner_before.ok());

  const std::array moved{0.012};
  ASSERT_TRUE(channel.update_render_positions(binding, names, moved).ok());
  auto display = channel.current_bound_render_state();
  ASSERT_TRUE(display.ok());
  EXPECT_TRUE(display.value().display_only);
  EXPECT_EQ(display.value().positions_si, std::vector<double>{0.012});
  EXPECT_EQ(channel.current_state(100).value(), owner_before.value());

  // The same coordinates from an addressed sample are still a visible change.
  const auto generation = channel.render_change_generation();
  ASSERT_TRUE(channel.publish_state(moved, velocity, 101, 101).applied());
  display = channel.current_bound_render_state();
  ASSERT_TRUE(display.ok());
  EXPECT_FALSE(display.value().display_only);
  EXPECT_GT(channel.render_change_generation(), generation);
}

TEST(RetainedObservedStateChannelTest,
     AFollowingChannelKeepsTheDisplayInputOverAddressedSamples) {
  // M6 (approval list 2, item 23): a Real arm's display always follows its
  // vendor joint topic. Addressed samples render until the first display
  // input; afterwards they update only the authoritative state.
  const auto binding =
      make_binding(CoordinateKind::kLinear, "gripper.target", true);
  RetainedObservedStateChannel channel(
      binding, nullptr,
      RetainedObservedStateChannelConfig{
          ObservedStateSourceKind::kPredictedState, "native.predicted", 0, 1U});
  channel.follow_display_input();
  const auto names = channel.query_context().joint_names;
  const std::array initial{0.0}, velocity{0.0};
  ASSERT_TRUE(channel.publish_state(initial, velocity, 100, 100).applied());
  auto render = channel.current_bound_render_state();
  ASSERT_TRUE(render.ok());
  EXPECT_FALSE(render.value().display_only);

  const std::array vendor{0.012};
  ASSERT_TRUE(channel.update_render_positions(binding, names, vendor).ok());
  const auto generation = channel.render_change_generation();
  const std::array addressed{0.02};
  ASSERT_TRUE(channel.publish_state(addressed, velocity, 101, 101).applied());
  render = channel.current_bound_render_state();
  ASSERT_TRUE(render.ok());
  EXPECT_TRUE(render.value().display_only);
  EXPECT_EQ(render.value().positions_si, std::vector<double>{0.012});
  EXPECT_EQ(channel.render_change_generation(), generation);
  // Planning and readiness read the addressed sample.
  const auto owner = channel.current_state(101);
  ASSERT_TRUE(owner.ok());
  EXPECT_DOUBLE_EQ(
      std::get<LinearPositionDTO>(owner.value().positions.front()).value_m, 0.02);

  const std::array next{0.015};
  ASSERT_TRUE(channel.update_render_positions(binding, names, next).ok());
  render = channel.current_bound_render_state();
  ASSERT_TRUE(render.ok());
  EXPECT_EQ(render.value().positions_si, std::vector<double>{0.015});
}

TEST(RetainedObservedStateChannelTest,
     ALivePreviewStartsFromTheDisplayInputOnlyWhileTheStreamLeads) {
  // M6 (approval list 2, item 34): poses follow the live controller stream.
  // A jog/Move-to preview starts from the display input (a Real arm's latest
  // valid vendor joint sample) while the arm counts as moved or re-arming,
  // or while the broadcaster's addressed state is stale. Otherwise, and on a
  // channel without display input, it starts from the addressed state.
  auto value = make_binding(CoordinateKind::kAngular, "arm.target").value();
  value.model_groups.clear();
  for (auto& resource : value.resources) resource.source_joint_name.reset();
  value.control_path->state_authority_class = StateAuthorityClass::kMeasured;
  const auto sealed = verify_runtime_participant_binding(std::move(value));
  ASSERT_TRUE(sealed.ok());
  RetainedObservedStateChannel channel(
      sealed.value(), nullptr,
      RetainedObservedStateChannelConfig{ObservedStateSourceKind::kJointState,
                                         "joint_state", 100, 1U});
  const auto names = channel.query_context().joint_names;
  const std::vector<double> broadcaster{0.1, 0.2}, vendor{0.4, 0.5}, zero{0.0, 0.0};
  ASSERT_TRUE(channel.publish_adapter_state(ObservedRobotStateSourceKind::kJointState,
      names, broadcaster, zero, 1000, 1000).complete);

  // Not display-led (Mock, Isaac, a vendor without a joint topic): never.
  ASSERT_TRUE(channel.update_render_positions(sealed.value(), names, vendor).ok());
  channel.set_display_input_leads(true);
  EXPECT_FALSE(channel.live_preview_start(1050).has_value());
  EXPECT_FALSE(channel.live_preview_start(2000).has_value());

  channel.follow_display_input();
  ASSERT_TRUE(channel.update_render_positions(sealed.value(), names, vendor).ok());
  // A normal preview: fresh broadcaster, arm not moved. Unchanged.
  channel.set_display_input_leads(false);
  EXPECT_FALSE(channel.live_preview_start(1050).has_value());
  // Moved or re-arming: the vendor pose, even while the broadcaster is fresh.
  channel.set_display_input_leads(true);
  auto live = channel.live_preview_start(1050);
  ASSERT_TRUE(live.has_value());
  EXPECT_EQ(live->compiled_joint_names, names);
  EXPECT_EQ(live->positions_si, vendor);
  EXPECT_EQ(live->binding.value(), sealed.value().value());
  // Broadcaster stale (past the 100 ns budget), not moved: the vendor pose.
  channel.set_display_input_leads(false);
  live = channel.live_preview_start(1200);
  ASSERT_TRUE(live.has_value());
  EXPECT_EQ(live->positions_si, vendor);
  // A fresh broadcaster sample ends it.
  ASSERT_TRUE(channel.publish_adapter_state(ObservedRobotStateSourceKind::kJointState,
      names, broadcaster, zero, 1300, 1300).complete);
  EXPECT_FALSE(channel.live_preview_start(1310).has_value());
  // Planning, readiness and arrival read the addressed state throughout.
  const auto owner = channel.current_state(1310);
  ASSERT_TRUE(owner.ok());
  EXPECT_DOUBLE_EQ(std::get<AngularPositionDTO>(owner.value().positions[0]).value_rad, 0.1);
}

TEST(RetainedObservedStateChannelTest,
     PresentationReadsFollowTheDisplayInputWhileTheStreamLeads) {
  // M6 (approval list 2, items 23/34): the host's control-state and
  // observation reads serve the live display pose under the preview rule,
  // instead of failing on a stale broadcaster. Otherwise they are exactly
  // current_bound_state; current_state (planning) never changes.
  auto value = make_binding(CoordinateKind::kAngular, "arm.target").value();
  value.model_groups.clear();
  for (auto& resource : value.resources) resource.source_joint_name.reset();
  value.control_path->state_authority_class = StateAuthorityClass::kMeasured;
  const auto sealed = verify_runtime_participant_binding(std::move(value));
  ASSERT_TRUE(sealed.ok());
  RetainedObservedStateChannel channel(
      sealed.value(), nullptr,
      RetainedObservedStateChannelConfig{ObservedStateSourceKind::kJointState,
                                         "joint_state", 100, 1U});
  const auto names = channel.query_context().joint_names;
  const std::vector<double> broadcaster{0.1, 0.2}, vendor{0.4, 0.5}, moving{0.3, 0.3};
  ASSERT_TRUE(channel.publish_adapter_state(ObservedRobotStateSourceKind::kJointState,
      names, broadcaster, moving, 1000, 1000).complete);
  const auto position = [](const ObservedStateSnapshotDTO& state, std::size_t index) {
    return std::get<AngularPositionDTO>(state.positions[index]).value_rad;
  };

  // Not display-led: exactly current_bound_state, which fails when stale.
  ASSERT_TRUE(channel.update_render_positions(sealed.value(), names, vendor).ok());
  channel.set_display_input_leads(true);
  auto read = channel.current_live_bound_state(1050);
  ASSERT_TRUE(read.ok());
  EXPECT_DOUBLE_EQ(position(read.value().state, 0), 0.1);
  EXPECT_FALSE(channel.current_live_bound_state(1200).ok());

  channel.follow_display_input();
  ASSERT_TRUE(channel.update_render_positions(sealed.value(), names, vendor).ok());
  // Fresh broadcaster, not moved: the addressed state, unchanged.
  channel.set_display_input_leads(false);
  read = channel.current_live_bound_state(1050);
  ASSERT_TRUE(read.ok());
  EXPECT_DOUBLE_EQ(position(read.value().state, 0), 0.1);
  EXPECT_TRUE(read.value().state.velocities.has_value());
  // Stale broadcaster: the live vendor pose, with the addressed identity.
  ASSERT_FALSE(channel.current_bound_state(1200).ok());
  read = channel.current_live_bound_state(1200);
  ASSERT_TRUE(read.ok()) << read.error().message;
  EXPECT_EQ(read.value().binding.value(), sealed.value().value());
  EXPECT_EQ(read.value().state.joint_names, names);
  EXPECT_DOUBLE_EQ(position(read.value().state, 0), 0.4);
  EXPECT_DOUBLE_EQ(position(read.value().state, 1), 0.5);
  EXPECT_TRUE(read.value().state.complete);
  EXPECT_FALSE(read.value().state.velocities.has_value());
  EXPECT_EQ(read.value().state.control_binding_id,
            channel.query_context().control_binding_id);
  // Moved or re-arming with a fresh broadcaster: the live vendor pose too.
  channel.set_display_input_leads(true);
  read = channel.current_live_bound_state(1050);
  ASSERT_TRUE(read.ok());
  EXPECT_DOUBLE_EQ(position(read.value().state, 0), 0.4);
  // Planning, readiness and arrival never read it.
  const auto owner = channel.current_state(1050);
  ASSERT_TRUE(owner.ok());
  EXPECT_DOUBLE_EQ(position(owner.value(), 0), 0.1);
  EXPECT_FALSE(channel.current_state(1200).ok());
}

TEST(RetainedObservedStateChannelTest,
     RenderWritesRejectForeignBindingNamesAndNonfiniteValues) {
  const auto binding = make_binding(CoordinateKind::kLinear, "gripper.target",
                                    true, "route.native");
  const auto rebound = make_binding(CoordinateKind::kLinear, "gripper.target",
                                    true, "route.successor");
  RetainedObservedStateChannel channel(
      binding, nullptr,
      RetainedObservedStateChannelConfig{
          ObservedStateSourceKind::kPredictedState, "native.predicted", 0, 1U});
  const auto names = channel.query_context().joint_names;
  const std::array initial{0.0}, velocity{0.0}, moved{0.025};
  ASSERT_TRUE(channel.publish_state(initial, velocity, 100, 100).applied());
  ASSERT_TRUE(channel.RebindRoute(rebound, 101).ok());
  const auto generation = channel.render_change_generation();
  EXPECT_FALSE(channel.update_render_positions(binding, names, moved).ok());
  const auto foreign =
      make_binding(CoordinateKind::kLinear, "foreign.target", true);
  EXPECT_FALSE(channel.update_render_positions(foreign, names, moved).ok());
  auto foreign_model_dto = rebound.value();
  foreign_model_dto.model_bindings.front().model_revision =
      ModelRevision("foreign-model-revision");
  for (auto &frame : foreign_model_dto.frame_bindings)
    frame.model_revision = ModelRevision("foreign-model-revision");
  const auto foreign_model =
      verify_runtime_participant_binding(std::move(foreign_model_dto));
  ASSERT_TRUE(foreign_model.ok());
  EXPECT_FALSE(channel.update_render_positions(foreign_model.value(), names,
                                              moved).ok());
  EXPECT_FALSE(channel.update_render_positions(
                          rebound, std::vector<std::string>{"foreign.finger"},
                          moved).ok());
  EXPECT_FALSE(channel.update_render_positions(rebound, {}, {}).ok());
  const std::array nonfinite{std::numeric_limits<double>::quiet_NaN()};
  EXPECT_FALSE(
      channel.update_render_positions(rebound, names, nonfinite).ok());
  EXPECT_EQ(channel.render_change_generation(), generation);
  const auto preserved = channel.current_bound_render_state();
  ASSERT_TRUE(preserved.ok());
  EXPECT_EQ(preserved.value().binding.value(), rebound.value());
  EXPECT_EQ(preserved.value().positions_si, std::vector<double>{0.0});
  ASSERT_TRUE(channel.update_render_positions(rebound, names, moved).ok());
}

TEST(RetainedObservedStateChannelTest,
     RenderWritesReorderExactNamesAndRejectPartialOrDuplicateSamples) {
  auto dto = make_binding(CoordinateKind::kLinear, "gripper.target", true)
                 .value();
  auto second = dto.resources.front();
  second.resource_id = ResourceId("resource.finger.second");
  second.resource_role_id = ResourceRoleId("second_finger");
  second.compiled_resource_name = "resource.finger.second";
  dto.resources.push_back(std::move(second));
  const auto binding = verify_runtime_participant_binding(std::move(dto));
  ASSERT_TRUE(binding.ok());
  RetainedObservedStateChannel channel(
      binding.value(), nullptr,
      RetainedObservedStateChannelConfig{
          ObservedStateSourceKind::kPredictedState, "native.predicted", 0, 1U});
  const auto canonical_names = channel.query_context().joint_names;
  ASSERT_EQ(canonical_names.size(), 2U);
  const std::array reversed_names{canonical_names[1], canonical_names[0]};
  const std::array reversed_positions{0.02, 0.01};
  ASSERT_TRUE(channel.update_render_positions(binding.value(), reversed_names,
                                             reversed_positions).ok());
  const auto rendered = channel.current_bound_render_state();
  ASSERT_TRUE(rendered.ok());
  EXPECT_EQ(rendered.value().compiled_joint_names, canonical_names);
  EXPECT_EQ(rendered.value().positions_si,
            (std::vector<double>{0.01, 0.02}));
  const std::array duplicate_names{canonical_names[0], canonical_names[0]};
  EXPECT_FALSE(channel.update_render_positions(binding.value(), duplicate_names,
                                              reversed_positions).ok());
  EXPECT_FALSE(channel.update_render_positions(
                          binding.value(),
                          std::span<const std::string>(canonical_names).first(1),
                          std::span<const double>(reversed_positions).first(1))
                   .ok());
  EXPECT_EQ(channel.render_change_generation(),
            rendered.value().render_generation);
}

TEST(RetainedObservedStateChannelTest,
     RenderReadPairsCoordinatesAndGenerationDuringConcurrentUpdates) {
  const auto binding =
      make_binding(CoordinateKind::kLinear, "gripper.target", true);
  RetainedObservedStateChannel channel(
      binding, nullptr,
      RetainedObservedStateChannelConfig{
          ObservedStateSourceKind::kPredictedState, "native.predicted", 0, 1U});
  const auto names = channel.query_context().joint_names;
  const std::array initial{0.0};
  ASSERT_TRUE(channel.update_render_positions(binding, names, initial).ok());
  const auto initial_generation = channel.render_change_generation();
  std::atomic<bool> done{false};
  std::thread publisher([&] {
    for (std::uint64_t index = 1U; index <= 2000U; ++index) {
      const std::array position{static_cast<double>(index) * 0.001};
      EXPECT_TRUE(channel.update_render_positions(binding, names, position).ok());
    }
    done.store(true, std::memory_order_release);
  });
  std::size_t reads = 0U;
  while (!done.load(std::memory_order_acquire) || reads == 0U) {
    const auto rendered = channel.current_bound_render_state();
    const bool coherent =
        rendered.ok() && rendered.value().binding.value() == binding.value() &&
        rendered.value().compiled_joint_names == names &&
        rendered.value().positions_si.size() == 1U &&
        rendered.value().positions_si.front() ==
            static_cast<double>(rendered.value().render_generation -
                                initial_generation) * 0.001;
    EXPECT_TRUE(coherent);
    if (!coherent) break;
    ++reads;
  }
  publisher.join();
  EXPECT_GT(reads, 0U);
  EXPECT_FALSE(channel.current_state(100).ok());
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

TEST(ProgramResetPositionObservation, EventWaitRequiresConsumptionAcknowledgmentAndHonorsFailure) {
  RetainedObservationWake wake;
  int reads = 0;
  int validations = 0;
  const auto result = host_detail::WaitForMockResetObservation(wake, {},
      std::chrono::steady_clock::now() + std::chrono::seconds(1), [&] {
        ++reads;
        if (reads == 1) wake.notify(); // An existing source publication races the wait.
        return core::Result<bool>::Ok(reads == 2);
      }, [&] {
        ++validations;
        return core::Result<void>::Ok();
      });
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(reads, 2);
  EXPECT_EQ(validations, 2);
  const auto expired = host_detail::WaitForMockResetObservation(wake, {},
      std::chrono::steady_clock::now(), [] { return core::Result<bool>::Ok(false); },
      [] { return core::Result<void>::Ok(); });
  ASSERT_FALSE(expired.ok());
  EXPECT_EQ(expired.error().details.at("reason"), "reset_position_acknowledgment_timeout");
  const auto invalid = host_detail::WaitForMockResetObservation(wake, {},
      std::chrono::steady_clock::now() + std::chrono::seconds(1),
      [] { ADD_FAILURE() << "A revoked scope cannot read the source"; return core::Result<bool>::Ok(true); },
      [] { return core::Result<void>::Err({"scope_revoked", "fault", {}}); });
  ASSERT_FALSE(invalid.ok());
  EXPECT_EQ(invalid.error().code, "scope_revoked");
  auto controlled_now = std::chrono::steady_clock::time_point{};
  const auto controlled_deadline = controlled_now + std::chrono::seconds(1);
  const auto late = host_detail::WaitForMockResetObservation(wake, {}, controlled_deadline,
      [&] {
        controlled_now = controlled_deadline + std::chrono::nanoseconds(1);
        return core::Result<bool>::Ok(true);
      }, [] { return core::Result<void>::Ok(); }, [&] { return controlled_now; });
  ASSERT_FALSE(late.ok());
  EXPECT_EQ(late.error().details.at("reason"), "reset_position_acknowledgment_timeout");
  std::stop_source stop;
  stop.request_stop();
  const auto cancelled = host_detail::WaitForMockResetObservation(wake, stop.get_token(),
      std::chrono::steady_clock::now() + std::chrono::seconds(1),
      [] { ADD_FAILURE() << "Cancellation cannot read the source"; return core::Result<bool>::Ok(true); },
      [] { return core::Result<void>::Ok(); });
  ASSERT_FALSE(cancelled.ok());
  EXPECT_EQ(cancelled.error().details.at("reason"), "reset_position_cancelled");
}

TEST(ProgramResetPositionObservation, SameInitialTargetStillNeedsPostPublishMeasuredEvidence) {
  // This unit fixture exercises the publication fence without a planning
  // model. Keep its two ordered angular resources explicitly component-local.
  auto value = make_binding(CoordinateKind::kAngular, "arm.target").value();
  value.model_groups.clear();
  for (auto& resource : value.resources) resource.source_joint_name.reset();
  value.control_path->state_authority_class = StateAuthorityClass::kMeasured;
  const auto binding = verify_runtime_participant_binding(std::move(value));
  ASSERT_TRUE(binding.ok());
  RetainedObservedStateChannel channel(binding.value(), nullptr,
      RetainedObservedStateChannelConfig{ObservedStateSourceKind::kJointState,
                                         "joint_state", 100, 1U});
  ASSERT_TRUE(channel.publish_adapter_state(ObservedRobotStateSourceKind::kJointState,
      channel.query_context().joint_names, std::vector<double>{0.0, 0.0},
      std::vector<double>{0.0, 0.0}, 12, 22).complete);
  auto captured = channel.current_state(22);
  ASSERT_TRUE(captured.ok());
  auto measured = std::move(captured.value());
  measured.sequence = 4U;
  measured.source_timestamp_ns = 12;
  measured.received_timestamp_ns = 22;
  measured.positions = {core::AngularPositionDTO{0.0}, core::AngularPositionDTO{0.0}};
  const std::vector<double> target{0.0, 0.0};
  // Velocity is deliberately irrelevant: this acknowledges the write, not
  // physical settling, a reference match or a zero-velocity dwell.
  measured.velocities = std::vector<core::CoordinateVelocityDTO>{core::AngularVelocityDTO{12.0}, core::AngularVelocityDTO{12.0}};
  EXPECT_TRUE(host_detail::ExactMockResetMeasuredTarget(measured, target, 3U, 11, 21));
  measured.positions = {core::AngularPositionDTO{0.01}, core::AngularPositionDTO{0.0}};
  EXPECT_FALSE(host_detail::ExactMockResetMeasuredTarget(measured, target, 3U, 11, 21));
  measured.positions = {core::AngularPositionDTO{0.0}, core::AngularPositionDTO{0.0}};
  EXPECT_FALSE(host_detail::ExactMockResetMeasuredTarget(measured, target, 4U, 11, 21));
  EXPECT_FALSE(host_detail::ExactMockResetMeasuredTarget(measured, target, 3U, 12, 21));
  EXPECT_FALSE(host_detail::ExactMockResetMeasuredTarget(measured, target, 3U, 11, 22));
  measured.positions = {core::LinearPositionDTO{0.0}, core::AngularPositionDTO{0.0}};
  EXPECT_FALSE(host_detail::ExactMockResetMeasuredTarget(measured, target, 3U, 11, 21));
}

TEST(RetainedObservedStateChannelTest,
     CommandedInitializationRejectsDelayedPredecessorsAndRetainsCutoffAcrossRebind) {
  // This unit fixture exercises the publication fence without a planning
  // model. Keep its two ordered angular resources explicitly component-local.
  auto value = make_binding(CoordinateKind::kAngular, "arm.target").value();
  value.model_groups.clear();
  for (auto& resource : value.resources) resource.source_joint_name.reset();
  value.control_path->state_authority_class = StateAuthorityClass::kCommanded;
  const auto sealed = verify_runtime_participant_binding(value);
  ASSERT_TRUE(sealed.ok());
  RetainedObservedStateChannel channel(sealed.value(), nullptr,
      RetainedObservedStateChannelConfig{ObservedStateSourceKind::kCommandedState,
                                         "commanded_state", 0, 1U});
  const auto names = channel.query_context().joint_names;
  const std::vector<double> before{0.1, 0.2}, target{1.0, -2.0}, zero{0.0, 0.0};
  ASSERT_TRUE(channel.publish_adapter_state(ObservedRobotStateSourceKind::kCommandedState,
      names, before, zero, 100, 100).complete);
  ASSERT_TRUE(channel.publish_commanded_initialization_and_project(target, zero, 200).applied());
  for (const auto stamp : {100, 200}) {
    const auto delayed = channel.publish_adapter_state(ObservedRobotStateSourceKind::kCommandedState,
        names, before, zero, stamp, 250);
    EXPECT_FALSE(delayed.published);
    EXPECT_STREQ(delayed.reason, "commanded_sample_precedes_backend_initialization");
  }
  // The addressed publication retains the command despite delayed delivery.
  auto current = channel.publication().current_state(channel.query_context(), 250);
  ASSERT_TRUE(current.ok());
  EXPECT_DOUBLE_EQ(std::get<AngularPositionDTO>(current.value().positions[0]).value_rad, target[0]);
  auto rebound_value = value;
  rebound_value.control_path->control_route_id = ControlRouteId("route.successor");
  rebound_value.control_path->runtime_scale_qualification->control_route_id = ControlRouteId("route.successor");
  const auto rebound = verify_runtime_participant_binding(std::move(rebound_value));
  ASSERT_TRUE(rebound.ok());
  ASSERT_TRUE(channel.RebindRoute(rebound.value(), 300).ok());
  EXPECT_FALSE(channel.publish_adapter_state(ObservedRobotStateSourceKind::kCommandedState,
      names, before, zero, 150, 400).published);
  // A genuine later command flows, including motion derivatives/different pose.
  const std::vector<double> moving{0.2, 0.3};
  EXPECT_TRUE(channel.publish_adapter_state(ObservedRobotStateSourceKind::kCommandedState,
      names, before, moving, 301, 401).complete);
  current = channel.current_state(401);
  ASSERT_TRUE(current.ok());
  EXPECT_DOUBLE_EQ(std::get<AngularPositionDTO>(current.value().positions[0]).value_rad, before[0]);
}

TEST(RetainedObservedStateChannelTest,
     CommandedInitializationDoesNotChangeOrdinaryOrMeasuredPublicationPolicy) {
  // This unit fixture exercises the publication fence without a planning
  // model. Keep its two ordered angular resources explicitly component-local.
  auto value = make_binding(CoordinateKind::kAngular, "arm.target").value();
  value.model_groups.clear();
  for (auto& resource : value.resources) resource.source_joint_name.reset();
  value.control_path->state_authority_class = StateAuthorityClass::kCommanded;
  auto sealed = verify_runtime_participant_binding(value);
  ASSERT_TRUE(sealed.ok());
  RetainedObservedStateChannel ordinary(sealed.value(), nullptr,
      RetainedObservedStateChannelConfig{ObservedStateSourceKind::kCommandedState, "commanded_state", 0, 1U});
  const auto names = ordinary.query_context().joint_names;
  const std::vector<double> pose{0.1, 0.2}, zero{0.0, 0.0};
  ASSERT_TRUE(ordinary.publish_adapter_state(ObservedRobotStateSourceKind::kCommandedState,
      names, pose, zero, 200, 200).complete);
  EXPECT_TRUE(ordinary.publish_adapter_state(ObservedRobotStateSourceKind::kCommandedState,
      names, pose, zero, 100, 250).complete);  // No initialization cutoff was installed.
  EXPECT_FALSE(ordinary.publish_commanded_initialization_and_project(pose, pose, 300).applied());
  value.control_path->state_authority_class = StateAuthorityClass::kMeasured;
  sealed = verify_runtime_participant_binding(std::move(value));
  ASSERT_TRUE(sealed.ok());
  RetainedObservedStateChannel measured(sealed.value(), nullptr,
      RetainedObservedStateChannelConfig{ObservedStateSourceKind::kJointState, "joint_state", 100, 1U});
  EXPECT_FALSE(measured.publish_commanded_initialization_and_project(pose, zero, 300).applied());
  EXPECT_TRUE(measured.publish_adapter_state(ObservedRobotStateSourceKind::kJointState,
      names, pose, zero, 100, 300).complete);
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

class GripperRuntimeOwnerTestBackend final : public IGripperCommandBackend,
                                           public IControlDomainExecutionPort {
 public:
  explicit GripperRuntimeOwnerTestBackend(
      std::string gripper_id,
      GripperActuationKind actuation = GripperActuationKind::kPositionCommanded,
      GripperOutputMode output = GripperOutputMode::kCommanded)
      : actuation_(actuation), output_(output) {
    status.gripper_id = std::move(gripper_id);
  }

  GripperActuationKind gripper_actuation_kind(const std::string&) const override {
    return actuation_;
  }
  GripperOutputMode gripper_output_mode(const std::string&) const override {
    return output_;
  }

  Result<void> validate_gripper_command(const GripperCommandRequestDTO&) const override {
    ++validate_calls;
    return Result<void>::Ok();
  }

  GripperCommandResultDTO command_gripper(const GripperCommandRequestDTO&) override {
    ++command_calls;
    if (!accept_commands) return {};
    std::lock_guard lock(mutex);
    status.state = GripperCommandState::kCompleted;
    status.command_id = 1U;
    status.generation = 1U;
    return {true, status.command_id, status.generation, std::nullopt};
  }

  mutable std::atomic<int> validate_calls{};
  std::atomic<int> command_calls{};
  bool accept_commands{};

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

  ParticipantStopStageResultDTO TranslateStopStage(
      const ResolvedParticipantStopStageDTO&, const RemainingStopBudgetDTO&,
      std::stop_token) override {
    throw std::logic_error("gripper owner fixture does not execute Stop stages");
  }

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
  const GripperActuationKind actuation_;
  const GripperOutputMode output_;
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
  EXPECT_EQ(before.stop_execution_port,
            static_cast<IControlDomainExecutionPort*>(owner.participant_engine()));
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

TEST(RetainedNativeGripperRuntimeTest, SelectedBackendOwnerRetainsSchedulerAndStopPort) {
  auto binding = make_binding(CoordinateKind::kLinear, "gripper.target", true);
  auto state_channel =
      std::make_shared<FakeRetainedStateChannel>(binding, std::vector<double>{0.0});
  auto clock = std::make_shared<FakeClock>();
  auto scheduler = std::make_shared<ManualScheduler>();
  auto selected_backend = std::make_shared<GripperRuntimeOwnerTestBackend>(
      binding.value().command_target_id.value());
  const std::weak_ptr<GripperRuntimeOwnerTestBackend> backend_lifetime = selected_backend;
  const std::weak_ptr<ManualScheduler> scheduler_lifetime = scheduler;
  std::optional<RuntimeParticipantBinding> rebound_binding;
  std::shared_ptr<IRetainedObservedStateChannel> rebound_channel;

  auto owner = std::make_unique<RetainedNativeGripperRuntime>(
      RetainedNativeGripperRuntimeInput{
          binding, selected_backend, clock, state_channel, selected_backend.get(),
          std::nullopt, false,
          [&](const RuntimeParticipantBinding& candidate,
              const std::shared_ptr<IRetainedObservedStateChannel>& candidate_channel,
              const std::optional<RuntimeParticipantStopReadiness>& readiness) {
            EXPECT_FALSE(readiness.has_value());
            rebound_binding = candidate;
            rebound_channel = candidate_channel;
            return Result<void>::Ok();
          },
          scheduler, std::nullopt});
  selected_backend.reset();
  scheduler.reset();

  ASSERT_NE(owner, nullptr);
  auto* backend = dynamic_cast<GripperRuntimeOwnerTestBackend*>(owner->backend());
  ASSERT_NE(backend, nullptr);
  EXPECT_FALSE(backend_lifetime.expired());
  EXPECT_FALSE(scheduler_lifetime.expired());
  ASSERT_NE(owner->participant_engine(), nullptr);
  EXPECT_NE(dynamic_cast<GripperParticipantEngine*>(owner->registration().engine), nullptr);
  EXPECT_EQ(owner->state_channel(), state_channel);
  EXPECT_EQ(owner->stop_execution_port(), static_cast<IControlDomainExecutionPort*>(backend));
  EXPECT_EQ(owner->registration().stop_execution_port, owner->stop_execution_port());

  const auto candidate = make_next_publication_gripper_binding(binding);
  auto candidate_channel =
      std::make_shared<FakeRetainedStateChannel>(candidate, std::vector<double>{0.0});
  ASSERT_TRUE(owner->RebindRoute(candidate, candidate_channel, std::nullopt).ok());
  ASSERT_TRUE(rebound_binding.has_value());
  EXPECT_EQ(rebound_binding->value(), candidate.value());
  EXPECT_EQ(rebound_channel, candidate_channel);
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
           const std::vector<RuntimeParticipantExecutionRegistration>& registrations) {
          RetainedNativeGripperRoutePublicationOwner::KernelCandidate candidate;
          candidate.registrations = registrations;
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
           const std::vector<RuntimeParticipantExecutionRegistration>&) {
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

    const auto routes_before_activation = rebound_routes;
    const auto builders_before_activation = builder_calls;
    const auto rows_before_activation = gripper->registrations();
    const auto channel_before_activation = gripper->state_channel();
    const auto context_before_activation = state_channel->query_context();
    const auto admission_before_activation = kernel.GetAuthoringAdmission();
    EXPECT_FALSE(admission_before_activation.authoring_allowed);
    auto changed_activation = predecessor;
    changed_activation.activation_set_id = ActivationSetId("activation.foreign");
    const auto refused_activation =
        owner.AdoptAndReadBackRouteConfigurationPredecessor(changed_activation);
    ASSERT_FALSE(refused_activation.ok());
    EXPECT_EQ(refused_activation.error().code, "route_predecessor_gripper_rebind_failed");
    ASSERT_TRUE(refused_activation.error().details.contains("reason"));
    EXPECT_EQ(refused_activation.error().details.at("reason"),
              "native_gripper_alias_owner_mismatch");
    EXPECT_EQ(refused_activation.error().message,
              "Gripper representatives lost their exact primary command owner");
    EXPECT_EQ(builder_calls, 1);
    EXPECT_EQ(builder_calls, builders_before_activation);
    EXPECT_EQ(rebound_routes, routes_before_activation);
    EXPECT_EQ(rebound_routes,
              (std::vector<ControlRouteId>{
                  predecessor.binding_routes.front().control_route_id,
                  current_binding.value().control_path->control_route_id}));
    EXPECT_EQ(gripper->binding().value(), current_binding.value());
    EXPECT_EQ(gripper->state_channel(), channel_before_activation);
    EXPECT_EQ(gripper->state_channel(), state_channel);
    EXPECT_EQ(state_channel->query_context(), context_before_activation);
    EXPECT_EQ(state_channel->query_context(),
              make_observed_state_query_context(
                  current_binding,
                  state_channel->query_context().freshness_budget_ns)
                  .value());
    const auto rows_after_activation = gripper->registrations();
    ASSERT_EQ(rows_before_activation.size(), 1U);
    ASSERT_EQ(rows_after_activation.size(), rows_before_activation.size());
    const auto& before_row = rows_before_activation.front();
    const auto& after_row = rows_after_activation.front();
    EXPECT_EQ(after_row.binding.value(), before_row.binding.value());
    EXPECT_EQ(after_row.engine, before_row.engine);
    EXPECT_EQ(after_row.stop_execution_port, before_row.stop_execution_port);
    EXPECT_EQ(after_row.prepared_motion_engine, before_row.prepared_motion_engine);
    EXPECT_EQ(after_row.stop_node_category, before_row.stop_node_category);
    EXPECT_EQ(after_row.intrinsic_direct_control, before_row.intrinsic_direct_control);
    EXPECT_EQ(after_row.deployment_maximum_tcp_linear_speed_mps,
              before_row.deployment_maximum_tcp_linear_speed_mps);
    EXPECT_TRUE(before_row.retained_default_stop_limits.empty());
    EXPECT_TRUE(after_row.retained_default_stop_limits.empty());
    EXPECT_FALSE(before_row.stop_readiness);
    EXPECT_FALSE(after_row.stop_readiness);
    EXPECT_FALSE(before_row.participant_policy);
    EXPECT_FALSE(after_row.participant_policy);
    EXPECT_FALSE(before_row.default_operator_stop_profile);
    EXPECT_FALSE(after_row.default_operator_stop_profile);
    EXPECT_FALSE(before_row.default_fault_stop_profile);
    EXPECT_FALSE(after_row.default_fault_stop_profile);
    EXPECT_TRUE(before_row.direct_control_joint_limits.empty());
    EXPECT_TRUE(after_row.direct_control_joint_limits.empty());
    EXPECT_EQ(kernel.GetAuthoringAdmission(), admission_before_activation);
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

  auto state_channel = std::make_shared<RetainedObservedStateChannel>(
      binding, nullptr,
      RetainedObservedStateChannelConfig{
          ObservedStateSourceKind::kPredictedState, "native.gripper.local",
          1'000, 1U},
      clock.get(), wake);
  auto selected_backend = std::make_shared<GripperRuntimeOwnerTestBackend>(
      binding.value().command_target_id.value());
  auto owner = std::make_unique<RetainedNativeGripperRuntime>(
      RetainedNativeGripperRuntimeInput{
          binding, selected_backend, clock, state_channel, selected_backend.get(),
          std::nullopt, false, {}, scheduler, std::nullopt});

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
  auto wake = std::make_shared<RetainedObservationWake>();

  EXPECT_THROW(
      RetainedObservedStateChannel(
          binding, nullptr,
          RetainedObservedStateChannelConfig{
              ObservedStateSourceKind::kPredictedState,
              "native.gripper.model", 1'000, 1U},
          clock.get(), wake),
      std::invalid_argument);
}

[[nodiscard]] RuntimeParticipantBinding seal_gripper_alias_test_binding(
    RuntimeParticipantBindingDTO value) {
  auto sealed = verify_runtime_participant_binding(std::move(value));
  if (!sealed.ok()) throw std::logic_error(sealed.error().message);
  return std::move(sealed.value());
}

[[nodiscard]] std::string gripper_alias_test_digest(char digit) {
  return "sha256:" + std::string(64U, digit);
}

[[nodiscard]] RuntimeParticipantBinding make_gripper_alias_command_binding() {
  auto value = make_binding(CoordinateKind::kLinear, "gripper.target", true).value();
  value.capability_id = CapabilityId("tool.gripper.command");
  // Canonical unit-fixture digests follow the existing Core Stop golden.
  // Generic make_binding remains a permissive binding-only fixture.
  value.program_revision = ProgramRevision(gripper_alias_test_digest('1'));
  value.program_binding_id = ProgramBindingId(gripper_alias_test_digest('2'));
  value.retained_bundle_id = RetainedBundleId(gripper_alias_test_digest('8'));
  value.graph_revision = GraphRevision(gripper_alias_test_digest('9'));
  value.control_path->control_domain_revision = ControlDomainRevision(gripper_alias_test_digest('c'));
  value.control_path->control_binding_revision = ControlBindingRevision(gripper_alias_test_digest('d'));
  value.control_path->topology_revision = TopologyRevision(gripper_alias_test_digest('e'));
  value.control_path->stop_evidence_policy_revision = StopEvidencePolicyRevision(gripper_alias_test_digest('f'));
  value.control_path->provider_manifest_revision = ProviderManifestRevision(gripper_alias_test_digest('2'));
  value.command_schema = ProgramCommandSchemaRefDTO{
      CommandSchemaId(std::string(kGripperPositionCommandSchemaId)),
      CommandSchemaRevision(std::string(kGripperPositionCommandSchemaRevision))};
  return seal_gripper_alias_test_binding(std::move(value));
}

[[nodiscard]] RuntimeParticipantBinding make_gripper_feedback_alias(
    const RuntimeParticipantBinding& command) {
  auto value = command.value();
  value.capability_id = CapabilityId("tool.gripper.feedback");
  value.command_schema.reset();
  value.capability_definition_revision = CapabilityDefinitionRevision("feedback-capability-revision");
  value.control_path->control_binding_id = ControlBindingId(
      command.value().control_path->control_binding_id.value() + ".feedback");
  value.control_path->control_binding_revision = ControlBindingRevision(gripper_alias_test_digest('a'));
  // The actual producer clones the same interface, target, resources and
  // physical owner. Feedback is another logical binding, not another engine.
  return seal_gripper_alias_test_binding(std::move(value));
}

[[nodiscard]] ActiveControlConfigurationDTO gripper_alias_configuration(
    const RuntimeParticipantBinding& command, const RuntimeParticipantBinding& feedback) {
  auto configuration = gripper_route_configuration(command);
  const auto& path = *feedback.value().control_path;
  configuration.binding_routes.push_back(
      {path.control_binding_id, path.control_binding_revision, path.control_route_id});
  std::sort(configuration.binding_routes.begin(), configuration.binding_routes.end(),
      [](const auto& left, const auto& right) { return left.control_binding_id < right.control_binding_id; });
  return configuration;
}

void expect_same_gripper_alias_rows(
    const std::vector<RuntimeParticipantExecutionRegistration>& actual,
    const std::vector<RuntimeParticipantExecutionRegistration>& expected) {
  ASSERT_EQ(actual.size(), expected.size());
  for (std::size_t index = 0; index < actual.size(); ++index) {
    EXPECT_EQ(actual[index].binding.value(), expected[index].binding.value());
    EXPECT_EQ(actual[index].engine, expected[index].engine);
    EXPECT_EQ(actual[index].stop_execution_port, expected[index].stop_execution_port);
    EXPECT_EQ(actual[index].intrinsic_direct_control, expected[index].intrinsic_direct_control);
    EXPECT_EQ(actual[index].stop_readiness.has_value(), expected[index].stop_readiness.has_value());
    if (actual[index].stop_readiness && expected[index].stop_readiness) {
      EXPECT_EQ(actual[index].stop_readiness->value(), expected[index].stop_readiness->value());
    }
  }
}

// This is the existing Core golden-plan unit fixture, requalified for each
// exact alias through public content-addressing/domain/readiness verifiers.
// It is not a physical controller readiness or live Deployment qualification.
[[nodiscard]] ResolvedStopBasePlanDTO gripper_alias_stop_plan_for(
    const RuntimeParticipantBinding& participant, const IGripperCommandBackend& backend) {
  const auto path =
      std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
      "manyforge_core" / "test" / "golden" / "resolved_stop_base_plan.v1.json";
  std::ifstream stream(path, std::ios::binary);
  std::string bytes{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
  if (bytes.ends_with('\n')) {
    bytes.pop_back();
  }
  auto decoded = DecodeResolvedStopBasePlan(bytes);
  if (!decoded.ok()) {
    throw std::logic_error(decoded.error().message);
  }

  auto plan = std::move(decoded.value());
  const auto& binding = participant.value();
  const auto& control = *binding.control_path;
  plan.resolved_stop_base_plan_id = ResolvedStopBasePlanId("pending");
  plan.retained_bundle_id = binding.retained_bundle_id;
  plan.graph_revision = binding.graph_revision;
  plan.control_domain_id = control.control_domain_id;
  plan.control_domain_revision = control.control_domain_revision;
  plan.control_binding_id = control.control_binding_id;
  plan.control_binding_revision = control.control_binding_revision;
  plan.controller_topology_id = control.controller_topology_id;
  plan.topology_revision = control.topology_revision;
  plan.control_route_id = control.control_route_id;
  plan.activation_set_id = control.activation_set_id;
  plan.active_control_configuration_id = control.active_control_configuration_id;
  plan.active_control_configuration_generation = control.active_control_configuration_generation;
  plan.source_runtime_unit_id = control.provider_runtime_unit_id;
  plan.source_runtime_epoch = control.provider_runtime_epoch;
  plan.stop_evidence_policy_id = control.stop_evidence_policy_id;
  plan.stop_evidence_policy_revision = control.stop_evidence_policy_revision;

  std::vector<ResourceId> resources;
  resources.reserve(binding.resources.size());
  for (const auto& resource : binding.resources) {
    resources.push_back(resource.resource_id);
  }
  for (auto& stage : plan.escalation_stages) {
    std::visit([&](auto& scope) { scope.activation_set_id = control.activation_set_id; },
               stage.containment_scope);
    const auto capability_target =
        stage.containment_command.command_kind == StopContainmentCommandKind::kRegistered ||
        stage.containment_command.command_kind ==
            StopContainmentCommandKind::kCapabilityCommandCancel ||
        stage.containment_command.command_kind == StopContainmentCommandKind::kSafeOutputCommand ||
        stage.containment_command.command_kind == StopContainmentCommandKind::kLogicalCancel;
    if (capability_target && !binding.supporting_capability_provider_paths.empty()) {
      const auto& provider = binding.supporting_capability_provider_paths.front();
      stage.command_target.provider_instance_id = provider.provider_instance_id;
      stage.command_target.provider_manifest_id = provider.provider_manifest_id;
      stage.command_target.provider_manifest_revision = provider.provider_manifest_revision;
      stage.command_target.runtime_unit_id = provider.provider_runtime_unit_id;
      stage.command_target.provider_runtime_epoch = provider.provider_runtime_epoch;
      stage.command_target.runtime_unit_epoch = provider.provider_runtime_epoch;
    } else {
      stage.command_target.provider_instance_id = control.provider_instance_id;
      stage.command_target.provider_manifest_id = control.provider_manifest_id;
      stage.command_target.provider_manifest_revision = control.provider_manifest_revision;
      stage.command_target.runtime_unit_id = control.provider_runtime_unit_id;
      stage.command_target.provider_runtime_epoch = control.provider_runtime_epoch;
      stage.command_target.runtime_unit_epoch = control.provider_runtime_epoch;
    }
  }
  plan.escalation_stages.front().containment_granularity = control.containment_granularity;
  plan.escalation_stages.front().containment_scope =
      ResolvedStopResourceContainmentScopeDTO{control.activation_set_id, resources};
  // Match the existing Core LogicalGripperRegistration fixture: this actual
  // local gripper engine supplies its own cancellation/effect fence. A
  // component-local scalar is not a fabricated motion-controller joint.
  auto& nominal = plan.escalation_stages.front();
  const ProviderContractRefDTO contract{
      ProviderContractRoleId("gripper_logical_cancel"),
      SchemaId("manyforge.value_schema.provider_contract.empty.v1"),
      SchemaRevision("sha256:fb466dd341076cbdb6fc97558ad28bc46032d25daaf4d4bb9d548bf642b83270")};
  nominal.containment_command = {StopContainmentCommandKind::kLogicalCancel, contract, {}};
  nominal.default_contract_parameters = JsonObjectDTO{};
  nominal.command_qualification = {};
  nominal.command_target.endpoint_role_id.reset();
  nominal.command_target.logical_endpoint_symbol.reset();
  nominal.evidence_requirements.erase(nominal.evidence_requirements.begin() + 1,
                                      nominal.evidence_requirements.end());
  auto& logical = nominal.evidence_requirements.front();
  logical.requirement = {
      StopEvidenceItemId("logical_cancel_fenced"),
      StopEvidenceRequirementKind::kLogicalCancellationNoContinuingEffect, contract, {},
      {StopEvidenceSourceKind::kCommandTargetStatus, 100'000'000LL, {}, {}},
      {StopEvidenceObservationKind::kCommandResult, {}}};
  logical.resolved_source = {StopEvidenceSourceKind::kCommandTargetStatus,
                            nominal.command_target, {}, {}, {}};
  const auto actuation = backend.gripper_actuation_kind(binding.command_target_id.value());
  if (actuation == GripperActuationKind::kPneumatic) {
    plan.gripper_actuation_kind = actuation;
    plan.escalation_stages.erase(plan.escalation_stages.begin() + 1,
                                plan.escalation_stages.end());
  }
  // Position retains the real golden hard stage; pneumatic has only its
  // source-qualified nominal fence. Seal the actual final scope in either case.
  plan.containment_granularity = plan.escalation_stages.back().containment_granularity;
  auto made = MakeResolvedStopBasePlan(std::move(plan));
  if (!made.ok()) {
    throw std::logic_error(made.error().message + ": " + made.error().details.at("reason"));
  }
  return std::move(made.value());
}

[[nodiscard]] ControllerLifecycleExpectedDomain gripper_alias_controller_domain_for(
    const RuntimeParticipantBinding& participant, const ResolvedStopBasePlanDTO& plan,
    const IGripperCommandBackend& backend,
    std::vector<RuntimeParticipantBinding> domain_members = {}) {
  // Every binding routed in the participant's domain. A single-component
  // domain has only the participant itself.
  if (domain_members.empty()) domain_members.push_back(participant);
  std::sort(domain_members.begin(), domain_members.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.value().control_path->control_binding_id <
           rhs.value().control_path->control_binding_id;
  });
  const auto& binding = participant.value();
  const auto& control = *binding.control_path;
  const ControllerRoleId controller_role("hardware");
  std::vector<ControllerInterfaceInstanceDTO> all_interfaces;
  std::vector<DesiredControlBindingRouteDTO> routes;
  std::vector<ControllerLifecycleExpectedBindingDTO> binding_interfaces;
  std::vector<ControllerLifecycleExpectedEndpointDTO> endpoint_bindings;
  for (const auto& member : domain_members) {
    const auto& path = *member.value().control_path;
    std::vector<ControllerInterfaceInstanceDTO> interfaces;
    for (const auto& resource : member.value().resources) {
      interfaces.push_back(ControllerInterfaceInstanceDTO{resource.resource_id, "position"});
    }
    all_interfaces.insert(all_interfaces.end(), interfaces.begin(), interfaces.end());
    routes.push_back({path.control_binding_id, path.control_binding_revision, path.control_route_id});
    binding_interfaces.push_back({path.control_binding_id, path.control_binding_revision,
                                  path.control_interface_contract_id,
                                  path.control_interface_revision, interfaces, interfaces});
    const auto actuation = backend.gripper_actuation_kind(member.value().command_target_id.value());
    if (actuation == GripperActuationKind::kPneumatic)
      binding_interfaces.back().gripper_actuation_kind = actuation;
    endpoint_bindings.push_back({path.control_binding_id, path.control_binding_revision,
                                 path.control_route_id, EndpointRoleId("endpoint.command"),
                                 "controller.command", true, false});
  }
  std::sort(all_interfaces.begin(), all_interfaces.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.resource_id < rhs.resource_id;
  });
  all_interfaces.erase(std::unique(all_interfaces.begin(), all_interfaces.end()),
                       all_interfaces.end());
  ControllerLifecycleExpectedDomainDTO value{
      control.control_domain_id,
      control.control_domain_revision,
      control.controller_topology_id,
      control.topology_revision,
      ProductId("test.control.product"),
      ProductContractId("manyforge.control-domain-control.v2"),
      ProductContractRevision(gripper_alias_test_digest('1')),
      ContentDigest(gripper_alias_test_digest('2')),
      ContentDigest(gripper_alias_test_digest('3')),
      plan.source_runtime_unit_id,
      plan.source_runtime_epoch,
      {control.controller_topology_id,
       control.topology_revision,
       {{controller_role,
         {{control.control_interface_contract_id,
           control.control_interface_revision,
           {ControllerClaimDirection::kCommand, ControllerClaimDirection::kState}}},
         {},
         {}}},
       {},
       {{control.activation_set_id, {controller_role}, {}}},
       {{control.control_route_id,
         control.control_interface_contract_id,
         control.control_interface_revision,
         {controller_role},
         {control.activation_set_id}}}},
      {control.control_domain_id,
       control.control_domain_revision,
       control.controller_topology_id,
       control.topology_revision,
       control.activation_set_id,
       std::move(routes)},
      std::move(binding_interfaces),
      {{controller_role,
        ControllerImplementationId("implementation.hardware"),
        ControllerImplementationRevision(gripper_alias_test_digest('4')),
        "controller_interface.ControllerInterface",
        ContentDigest(gripper_alias_test_digest('5')),
        all_interfaces,
        all_interfaces,
        {},
        {}}},
      std::move(endpoint_bindings)};
  auto sealed = verify_controller_lifecycle_expected_domain(std::move(value));
  if (!sealed.ok()) {
    throw std::logic_error(sealed.error().message);
  }
  return std::move(sealed.value());
}

TEST(RetainedNativeGripperRuntimeTest, CommandAndFeedbackAliasesKeepOneEngineAndOneCommandAuthority) {
  for (const auto output : {GripperOutputMode::kCommanded, GripperOutputMode::kNone}) {
    SCOPED_TRACE(output == GripperOutputMode::kNone ? "pneumatic None" : "pneumatic Commanded");
    const auto command = make_gripper_alias_command_binding();
    const auto feedback = make_gripper_feedback_alias(command);
    const std::vector<RuntimeParticipantBinding> representatives{command, feedback};
    auto backend = std::make_shared<GripperRuntimeOwnerTestBackend>(
        command.value().command_target_id.value(), GripperActuationKind::kPneumatic, output);
    auto clock = std::make_shared<FakeClock>();
    auto channel = std::make_shared<FakeRetainedStateChannel>(command, std::vector<double>{0.0});
    auto runtime = std::make_shared<RetainedNativeGripperRuntime>(RetainedNativeGripperRuntimeInput{
        command, backend, clock, channel,
        output == GripperOutputMode::kNone ? nullptr : backend.get(),
        std::nullopt, true, {}, {}, std::nullopt, {}, representatives});
    const auto primary = runtime->registration();
    const auto rows = runtime->registrations();
    ASSERT_EQ(rows.size(), 2U);
    EXPECT_EQ(primary.binding.value(), command.value());
    EXPECT_EQ(rows[0].binding.value(), command.value());
    EXPECT_EQ(rows[1].binding.value(), feedback.value());
    EXPECT_EQ(rows[0].engine, primary.engine);
    EXPECT_EQ(rows[1].engine, primary.engine);
    EXPECT_EQ(primary.engine, runtime->participant_engine());
    EXPECT_EQ(runtime->backend(), backend.get());
    EXPECT_TRUE(rows[0].intrinsic_direct_control);
    EXPECT_FALSE(rows[1].intrinsic_direct_control);
    EXPECT_FALSE(rows[0].stop_readiness);
    EXPECT_FALSE(rows[1].stop_readiness);
    EXPECT_EQ(rows[0].stop_execution_port, rows[1].stop_execution_port);
    EXPECT_EQ(rows[0].stop_execution_port,
              output == GripperOutputMode::kNone
                  ? static_cast<IControlDomainExecutionPort*>(runtime->participant_engine())
                  : static_cast<IControlDomainExecutionPort*>(backend.get()));
    const auto staged = runtime->PrepareRegistrations(representatives, {std::nullopt, std::nullopt});
    ASSERT_TRUE(staged.ok()) << staged.error().message;
    expect_same_gripper_alias_rows(staged.value(), rows);
    RetainedNativeComponentOwner component(runtime);
    const auto physical = component.physical_bindings();
    ASSERT_EQ(physical.size(), 1U);
    EXPECT_EQ(physical.front().value(), command.value());
    const auto logical = component.registrations();
    ASSERT_EQ(logical.size(), 2U);
    EXPECT_FALSE(logical[1].binding.value().command_schema);
    EXPECT_FALSE(logical[1].intrinsic_direct_control);
    expect_same_gripper_alias_rows(logical, rows);
  }
}

TEST(RetainedNativeGripperRuntimeTest, ForeignOrUnexpectedFeedbackCannotChangeTheInstalledAliasOwner) {
  const auto command = make_gripper_alias_command_binding();
  const auto feedback = make_gripper_feedback_alias(command);
  auto backend = std::make_shared<GripperRuntimeOwnerTestBackend>(command.value().command_target_id.value());
  auto clock = std::make_shared<FakeClock>();
  auto channel = std::make_shared<FakeRetainedStateChannel>(command, std::vector<double>{0.0});
  RetainedNativeGripperRuntime runtime(RetainedNativeGripperRuntimeInput{
      command, backend, clock, channel, backend.get(), std::nullopt, true, {}, {}, std::nullopt,
      {}, {command, feedback}});
  const auto before = runtime.registrations();
  for (int mutation = 0; mutation < 13; ++mutation) {
    SCOPED_TRACE(mutation);
    auto value = feedback.value();
    switch (mutation) {
      case 0: value.controllable_component_id = ControllableComponentId("foreign.component"); break;
      case 1: value.command_target_id = CommandTargetId("foreign.target"); break;
      case 2: value.resources[0].resource_id = ResourceId("foreign.resource"); break;
      case 3: value.control_path->provider_instance_id = ProviderInstanceId("foreign.provider"); break;
      case 4: value.control_path->provider_runtime_epoch = RuntimeEpoch("foreign.epoch");
              value.runtime_unit_epochs[0].runtime_epoch = RuntimeEpoch("foreign.epoch"); break;
      case 5: value.control_path->control_route_id = ControlRouteId("foreign.route"); break;
      case 6: value.control_path->active_control_configuration_generation = ActiveControlConfigurationGeneration(2U); break;
      case 7: value.control_path->control_binding_revision = ControlBindingRevision("foreign.binding.revision"); break;
      case 8: value.control_path->control_binding_id = ControlBindingId("unexpected.feedback"); break;
      case 9: value.capability_id = CapabilityId("tool.unexpected.feedback"); break;
      case 10: value.control_path->control_interface_contract_id = ControlInterfaceContractId("foreign.interface"); break;
      case 11: value.graph_revision = GraphRevision("foreign.graph"); break;
      case 12: value.control_path->active_control_configuration_id = ActiveControlConfigurationId("foreign.configuration"); break;
    }
    const auto foreign = seal_gripper_alias_test_binding(std::move(value));
    const auto refused = runtime.PrepareRegistrations({command, foreign}, {std::nullopt, std::nullopt});
    EXPECT_FALSE(refused.ok());
    expect_same_gripper_alias_rows(runtime.registrations(), before);
    EXPECT_EQ(runtime.binding().value(), command.value());
    EXPECT_EQ(runtime.state_channel(), channel);
  }
  EXPECT_FALSE(runtime.PrepareRegistrations({command, feedback, feedback},
      {std::nullopt, std::nullopt, std::nullopt}).ok());
  EXPECT_FALSE(runtime.PrepareRegistrations({command, feedback}, {std::nullopt}).ok());
  EXPECT_FALSE(runtime.PrepareRegistrations({feedback}, {std::nullopt}).ok());
  expect_same_gripper_alias_rows(runtime.registrations(), before);
}


TEST(RetainedNativeGripperRuntimeTest,
     FeedbackAddressedCommandIsRefusedBeforeBackendOrResourceReservation) {
  // Owning Core unit fixture, not retained Deployment provenance. The
  // configured backend supplies actuation independently to both seals.
  for (const auto configured_actuation : {GripperActuationKind::kPositionCommanded,
                                          GripperActuationKind::kPneumatic}) {
    SCOPED_TRACE(configured_actuation == GripperActuationKind::kPneumatic
                     ? "pneumatic" : "position_commanded");
    const auto command = make_gripper_alias_command_binding();
    const auto feedback = make_gripper_feedback_alias(command);
    ASSERT_TRUE(command.value().command_schema);
    ASSERT_FALSE(feedback.value().command_schema);
    const std::vector<RuntimeParticipantBinding> representatives{command, feedback};
    auto backend = std::make_shared<GripperRuntimeOwnerTestBackend>(
        command.value().command_target_id.value(), configured_actuation);
    const auto command_plan = gripper_alias_stop_plan_for(command, *backend);
    const auto expected = gripper_alias_controller_domain_for(command, command_plan, *backend, representatives);
    const auto actual_actuation = backend->gripper_actuation_kind(command.value().command_target_id.value());
    const bool pneumatic = actual_actuation == GripperActuationKind::kPneumatic;
    EXPECT_EQ(command_plan.gripper_actuation_kind.has_value(), pneumatic);
    if (pneumatic) {
      EXPECT_EQ(command_plan.gripper_actuation_kind, actual_actuation);
    }
    EXPECT_EQ(std::count_if(command_plan.escalation_stages.begin(), command_plan.escalation_stages.end(),
                           [](const auto& stage) { return stage.stage_kind == StopEscalationStageKind::kSoftwareHard; }),
              pneumatic ? 0 : 1);
    EXPECT_EQ(command_plan.containment_granularity,
              command_plan.escalation_stages.back().containment_granularity);
    EXPECT_EQ(command_plan.escalation_stages.front().containment_command.command_kind,
              StopContainmentCommandKind::kLogicalCancel);
    ASSERT_TRUE(command_plan.escalation_stages.front().default_contract_parameters);
    EXPECT_EQ(*command_plan.escalation_stages.front().default_contract_parameters, JsonObjectDTO{});
    for (const auto& binding : expected.value().expected_bindings) {
      EXPECT_EQ(binding.gripper_actuation_kind.has_value(), pneumatic);
      if (pneumatic) {
        EXPECT_EQ(binding.gripper_actuation_kind, actual_actuation);
      }
    }
    std::vector<std::optional<RuntimeParticipantStopReadiness>> readiness;
    for (const auto& binding : representatives) {
      auto verified = verify_runtime_participant_stop_readiness(
          {binding.value().controllable_component_id, ComponentLifecycleGeneration(1U),
           expected.value(), gripper_alias_stop_plan_for(binding, *backend)}, binding, expected);
      ASSERT_TRUE(verified.ok()) << verified.error().message;
      readiness.push_back(std::move(verified.value()));
    }
    auto clock = std::make_shared<FakeClock>();
    auto scheduler = std::make_shared<ManualScheduler>();
    backend->accept_commands = true;
    auto channel = std::make_shared<FakeRetainedStateChannel>(command, std::vector<double>{0.0});
    RetainedNativeGripperRuntime runtime(RetainedNativeGripperRuntimeInput{
        command, backend, clock, channel, backend.get(), readiness.front(), true,
        {}, scheduler, std::nullopt, {}, representatives});
    auto registrations = runtime.PrepareRegistrations(representatives, readiness);
    ASSERT_TRUE(registrations.ok()) << registrations.error().message;
    ASSERT_EQ(registrations.value().size(), 2U);
    EXPECT_EQ(registrations.value()[0].engine, registrations.value()[1].engine);
    EXPECT_FALSE(registrations.value()[1].intrinsic_direct_control);
    const auto cell = CellId("cell.gripper.alias.command");
    const auto epoch = RequestOwnerEpoch("owner.gripper.alias.command");
    const auto path = std::filesystem::temp_directory_path() /
        ("manyforge-gripper-alias-command-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) + ".sqlite");
    {
      SqliteOperationJournal journal(path, cell, 16U);
      // This synthetic owner has no external heartbeat receivers. As in the
      // Core intrinsic-manual tests, cell readiness is explicit owner input.
      ResourceAddressedExecutionKernelConfig config{
          epoch, cell, 16U, {}, nullptr, command.value().graph_revision};
      config.manager_liveness_required_set = ManagerLivenessRequiredSetDTO{
          cell, command.value().retained_bundle_id,
          ComputeRequiredRuntimeSetDigest({}), RuntimeUnitId("manager.gripper.alias"),
          RuntimeEpoch("manager.gripper.alias.1"),
          RequestOwnerEpoch("manager.gripper.alias.1"), {}};
      ResourceAddressedExecutionKernel kernel(registrations.value(), *clock, *scheduler,
          journal, config);
      ASSERT_FALSE(kernel.GetAuthoringAdmission().authoring_allowed);
      const CellAdmissionReadinessSnapshotDTO ready{
          true,
          ProgramCycleStatusDTO{RequestOwnerEpoch("program.owner.gripper.alias"), 1U,
                                ProgramCycleStateDTO::kIdleAfterReset, std::nullopt,
                                std::nullopt, std::nullopt},
          true, 1U, 1U, true, true, 0U, 0U, 0U};
      const auto admission = kernel.UpdateCellAdmissionReadiness(ready);
      ASSERT_TRUE(admission.ok()) << admission.error().message;
      ASSERT_TRUE(admission.value().authoring_allowed);
      const auto& value = command.value();
      const auto& schema = *value.command_schema;
      OperationIntentDTO intent{
          {RequestId("request.feedback.command"), epoch, "alias owner test", std::nullopt},
          value.program_id, value.program_revision, value.program_binding_id,
          ProgramRunId("run.gripper.alias"), value.graph_revision, value.retained_bundle_id,
          std::nullopt,
          {{value.component_role_id, value.target_role_id, feedback.value().capability_id,
            std::nullopt, CapabilityCommandIntentDTO{
                schema.command_schema_id, schema.command_schema_revision,
                GripperPositionCommandValueDTO{LinearPositionDTO{0.02}, std::nullopt},
                2.0, {}}}},
          OperationCoordinationPolicy::kIndependent};
      const auto before = kernel.resource_table_snapshot();
      const auto refused = kernel.execute(intent);
      ASSERT_FALSE(refused.ok());
      ASSERT_TRUE(refused.error().details.contains("reason"));
      EXPECT_EQ(refused.error().details.at("reason"), "runtime_command_mapping_incomplete");
      EXPECT_EQ(backend->validate_calls.load(), 0);
      EXPECT_EQ(backend->command_calls.load(), 0);
      EXPECT_EQ(kernel.resource_table_snapshot(), before);
      for (const auto& slot : kernel.resource_snapshot()) EXPECT_EQ(slot.state, ResourceSlotState::kFree);
      EXPECT_FALSE(runtime.participant_engine()->ObservePreparedCapabilityCommand(1U).ok());

      // The genuine command sibling remains registered with its exact schema;
      // the same real retained engine must prepare and dispatch it successfully.
      intent.request_context.request_id = RequestId("request.primary.command");
      intent.participants.front().required_capability_id = value.capability_id;
      const auto accepted = kernel.execute(intent);
      ASSERT_TRUE(accepted.ok()) << accepted.error().message;
      EXPECT_GT(backend->validate_calls.load(), 0);
      EXPECT_EQ(backend->command_calls.load(), 1);
      const auto prepared = runtime.participant_engine()->ObservePreparedCapabilityCommand(1U);
      ASSERT_TRUE(prepared.ok()) << prepared.error().message;
      EXPECT_EQ(prepared.value().state, ResolvedPreparedCapabilityCommandStateDTO::kDispatched);
      const auto replay = kernel.execute(intent);
      ASSERT_TRUE(replay.ok()) << replay.error().message;
      EXPECT_EQ(replay.value().operation_id, accepted.value().operation_id);
      EXPECT_EQ(backend->command_calls.load(), 1);
    }
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(path.string() + "-wal", ignored);
    std::filesystem::remove(path.string() + "-shm", ignored);
  }
}


[[nodiscard]] RuntimeParticipantBinding make_gripper_alias_peer_binding(
    const RuntimeParticipantBinding& command) {
  auto value = command.value();
  value.component_role_id = ComponentRoleId("peer");
  value.target_role_id = TargetRoleId("peer.gripper");
  value.controllable_component_id = ControllableComponentId("peer.component");
  value.command_target_id = CommandTargetId("peer.target");
  value.resources.front().resource_id = ResourceId("peer.finger");
  value.resources.front().compiled_resource_name = "peer.finger";
  auto& path = *value.control_path;
  path.control_domain_id = ControlDomainId("domain.peer");
  path.control_binding_id = ControlBindingId("binding.peer");
  path.control_route_id = ControlRouteId("route.peer");
  path.active_control_configuration_id = ActiveControlConfigurationId("active.peer");
  return seal_gripper_alias_test_binding(std::move(value));
}

TEST(RetainedNativeGripperRoutePublicationOwnerTest,
     AliasPredecessorReadbackPreservesBothBindingsAndUnaffectedCellPeer) {
  for (const auto output : {GripperOutputMode::kCommanded, GripperOutputMode::kNone}) {
    SCOPED_TRACE(output == GripperOutputMode::kNone ? "pneumatic None" : "pneumatic Commanded");
    const auto durable = make_gripper_alias_command_binding();
    const auto durable_feedback = make_gripper_feedback_alias(durable);
    const auto predecessor = gripper_alias_configuration(durable, durable_feedback);
    const auto current = make_restarted_gripper_binding(durable);
    const auto current_feedback = make_gripper_feedback_alias(current);
    const auto peer_binding = make_gripper_alias_peer_binding(current);
    auto clock = std::make_shared<FakeClock>();
    clock->set(100);
    auto scheduler = std::make_shared<ManualScheduler>();
    auto channel = make_gripper_restart_state_channel(current, *clock);
    auto backend = std::make_shared<GripperRuntimeOwnerTestBackend>(
        current.value().command_target_id.value(), GripperActuationKind::kPneumatic, output);
    std::vector<ControlRouteId> rebound_routes;
    auto runtime = std::make_shared<RetainedNativeGripperRuntime>(RetainedNativeGripperRuntimeInput{
        current, backend, clock, channel, nullptr, std::nullopt, true,
        [&](const RuntimeParticipantBinding& candidate,
            const std::shared_ptr<IRetainedObservedStateChannel>& actual_channel,
            const std::optional<RuntimeParticipantStopReadiness>& readiness) {
          EXPECT_EQ(actual_channel, channel);
          EXPECT_FALSE(readiness);
          rebound_routes.push_back(candidate.value().control_path->control_route_id);
          return Result<void>::Ok();
        }, scheduler, std::nullopt, {}, {current, current_feedback}});
    auto peer_backend = std::make_shared<GripperRuntimeOwnerTestBackend>("peer.target");
    auto peer_channel = make_gripper_restart_state_channel(peer_binding, *clock);
    RetainedNativeGripperRuntime peer(RetainedNativeGripperRuntimeInput{
        peer_binding, peer_backend, clock, peer_channel, nullptr, std::nullopt,
        true, {}, scheduler, std::nullopt});
    const auto peer_registration = peer.registration();
    auto installed = runtime->registrations();
    installed.push_back(peer_registration);
    const auto original_engine = runtime->participant_engine();
    const auto cell = CellId("cell.gripper.alias.recovery");
    const auto path = std::filesystem::temp_directory_path() /
        ("manyforge-gripper-alias-recovery-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) + ".sqlite");
    {
      SqliteOperationJournal journal(path, cell, 16U);
      ResourceAddressedExecutionKernel kernel(installed, *clock, *scheduler, journal,
          {RequestOwnerEpoch("owner.gripper.alias.recovery"), cell, 16U, {}, nullptr,
           current.value().graph_revision});
      auto peer_configuration = gripper_route_configuration(peer_binding);
      peer_configuration.state = ActiveControlConfigurationStateDTO::kExternalOrUnknownUnreconciled;
      const auto peer_before = kernel.publish_runtime_route_configuration(peer_configuration);
      ASSERT_TRUE(peer_before.ok()) << peer_before.error().message;
      int builder_calls = 0;
      int adopted_caches = 0;
      RetainedNativeGripperRoutePublicationOwner owner(runtime, *clock, kernel,
          [&](const ActiveControlConfigurationDTO& configuration,
              const std::vector<RuntimeParticipantExecutionRegistration>& aliases) {
            ++builder_calls;
            EXPECT_EQ(configuration, predecessor);
            EXPECT_EQ(aliases.size(), 2U);
            for (const auto& alias : aliases) {
              EXPECT_EQ(alias.engine, original_engine);
              EXPECT_FALSE(alias.stop_readiness);
              EXPECT_EQ(alias.binding.value().control_path->provider_runtime_epoch,
                        RuntimeEpoch("runtime-epoch.2"));
            }
            RetainedNativeGripperRoutePublicationOwner::KernelCandidate candidate;
            candidate.registrations = aliases;
            candidate.registrations.push_back(peer_registration);
            candidate.adopt_component_owners = [&] { ++adopted_caches; };
            return Result<RetainedNativeGripperRoutePublicationOwner::KernelCandidate>::Ok(
                std::move(candidate));
          });
      const auto current_configuration = gripper_alias_configuration(current, current_feedback);
      const auto read = owner.ReadCurrentPhysicalParticipants(current_configuration);
      ASSERT_TRUE(read.ok()) << read.error().message;
      ASSERT_EQ(read.value().size(), 2U);
      EXPECT_EQ(read.value()[0].value(), current.value());
      EXPECT_EQ(read.value()[1].value(), current_feedback.value());
      auto missing = current_configuration;
      missing.binding_routes.pop_back();
      EXPECT_FALSE(owner.ReadCurrentPhysicalParticipants(missing).ok());
      auto foreign = current_configuration;
      foreign.binding_routes.back().control_binding_revision = ControlBindingRevision("foreign.alias.revision");
      EXPECT_FALSE(owner.ReadCurrentPhysicalParticipants(foreign).ok());
      EXPECT_EQ(builder_calls, 0);

      ASSERT_TRUE(kernel.SetRouteRecoveryPending(true).ok());
      const auto adopted = owner.AdoptAndReadBackRouteConfigurationPredecessor(predecessor);
      ASSERT_TRUE(adopted.ok()) << adopted.error().message;
      EXPECT_EQ(builder_calls, 1);
      EXPECT_EQ(adopted_caches, 1);
      EXPECT_EQ(adopted.value().active_configuration, predecessor);
      EXPECT_TRUE(adopted.value().readiness_rows.empty());
      EXPECT_FALSE(adopted.value().operation_admission_open);
      EXPECT_EQ(rebound_routes, (std::vector<ControlRouteId>{ControlRouteId("route.native")}));
      const auto rows = runtime->registrations();
      ASSERT_EQ(rows.size(), 2U);
      for (const auto& row : rows) {
        EXPECT_EQ(row.engine, original_engine);
        EXPECT_FALSE(row.stop_readiness);
        EXPECT_EQ(row.binding.value().control_path->active_control_configuration_id,
                  predecessor.active_control_configuration_id);
        EXPECT_EQ(row.binding.value().control_path->active_control_configuration_generation,
                  predecessor.active_control_configuration_generation);
        EXPECT_EQ(row.binding.value().control_path->provider_runtime_epoch, RuntimeEpoch("runtime-epoch.2"));
        EXPECT_EQ(row.binding.value().control_path->control_route_id, ControlRouteId("route.native"));
      }
      EXPECT_TRUE(rows[0].intrinsic_direct_control);
      EXPECT_FALSE(rows[1].intrinsic_direct_control);
      EXPECT_FALSE(rows[1].binding.value().command_schema);
      EXPECT_EQ(rows[1].binding.value().control_path->control_binding_id,
                durable_feedback.value().control_path->control_binding_id);
      const auto readback = kernel.runtime_route_configuration_readback(
          predecessor.control_domain_id, predecessor.active_control_configuration_id,
          predecessor.active_control_configuration_generation);
      ASSERT_TRUE(readback.ok()) << readback.error().message;
      EXPECT_EQ(readback.value(), adopted.value());
      const auto peer_after = kernel.runtime_route_configuration_readback(
          peer_configuration.control_domain_id, peer_configuration.active_control_configuration_id,
          peer_configuration.active_control_configuration_generation);
      ASSERT_TRUE(peer_after.ok()) << peer_after.error().message;
      EXPECT_EQ(peer_after.value(), peer_before.value());
      EXPECT_EQ(peer.registration().binding.value(), peer_binding.value());
      EXPECT_EQ(peer.registration().engine, peer_registration.engine);
      EXPECT_EQ(peer_backend->command_calls.load(), 0);
      EXPECT_EQ(backend->command_calls.load(), 0);
      EXPECT_FALSE(kernel.GetAuthoringAdmission().authoring_allowed);
    }
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(path.string() + "-wal", ignored);
    std::filesystem::remove(path.string() + "-shm", ignored);
  }
}

TEST(RetainedNativeGripperRoutePublicationOwnerTest,
     FailedAliasCellCandidateRollsBackEveryAliasBeforeCacheAdoption) {
  const auto durable = make_gripper_alias_command_binding();
  const auto predecessor = gripper_alias_configuration(durable, make_gripper_feedback_alias(durable));
  const auto current = make_restarted_gripper_binding(durable);
  const auto feedback = make_gripper_feedback_alias(current);
  auto clock = std::make_shared<FakeClock>();
  clock->set(100);
  auto scheduler = std::make_shared<ManualScheduler>();
  auto channel = make_gripper_restart_state_channel(current, *clock);
  auto backend = std::make_shared<GripperRuntimeOwnerTestBackend>(current.value().command_target_id.value());
  std::vector<ControlRouteId> rebound_routes;
  auto runtime = std::make_shared<RetainedNativeGripperRuntime>(RetainedNativeGripperRuntimeInput{
      current, backend, clock, channel, nullptr, std::nullopt, true,
      [&](const RuntimeParticipantBinding& candidate,
          const std::shared_ptr<IRetainedObservedStateChannel>&,
          const std::optional<RuntimeParticipantStopReadiness>& readiness) {
        EXPECT_FALSE(readiness);
        rebound_routes.push_back(candidate.value().control_path->control_route_id);
        return Result<void>::Ok();
      }, scheduler, std::nullopt, {}, {current, feedback}});
  const auto before = runtime->registrations();
  const auto context_before = channel->query_context();
  const auto cell = CellId("cell.gripper.alias.failed.recovery");
  const auto path = std::filesystem::temp_directory_path() /
      ("manyforge-gripper-alias-failure-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()) + ".sqlite");
  {
    SqliteOperationJournal journal(path, cell, 16U);
    ResourceAddressedExecutionKernel kernel(before, *clock, *scheduler, journal,
        {RequestOwnerEpoch("owner.gripper.alias.failed.recovery"), cell, 16U, {}, nullptr,
         current.value().graph_revision});
    int builder_calls = 0;
    RetainedNativeGripperRoutePublicationOwner owner(runtime, *clock, kernel,
        [&](const ActiveControlConfigurationDTO&,
            const std::vector<RuntimeParticipantExecutionRegistration>& aliases) {
          ++builder_calls;
          EXPECT_EQ(aliases.size(), 2U);
          return Result<RetainedNativeGripperRoutePublicationOwner::KernelCandidate>::Err(
              {"test_alias_builder_failed", "injected complete-cell builder failure", {}});
        });
    ASSERT_TRUE(kernel.SetRouteRecoveryPending(true).ok());
    const auto refused = owner.AdoptAndReadBackRouteConfigurationPredecessor(predecessor);
    ASSERT_FALSE(refused.ok());
    ASSERT_TRUE(refused.error().details.contains("reason"));
    EXPECT_EQ(refused.error().details.at("reason"), "route_predecessor_gripper_builder_failed");
    EXPECT_EQ(builder_calls, 1);
    EXPECT_EQ(rebound_routes, (std::vector<ControlRouteId>{ControlRouteId("route.native"),
                                                        ControlRouteId("route.successor")}));
    expect_same_gripper_alias_rows(runtime->registrations(), before);
    EXPECT_EQ(runtime->binding().value(), current.value());
    EXPECT_EQ(runtime->state_channel(), channel);
    EXPECT_EQ(channel->query_context(), context_before);
    EXPECT_FALSE(kernel.runtime_route_configuration_readback(predecessor.control_domain_id,
        predecessor.active_control_configuration_id, predecessor.active_control_configuration_generation).ok());
    EXPECT_FALSE(kernel.GetAuthoringAdmission().authoring_allowed);
    EXPECT_EQ(backend->command_calls.load(), 0);
  }
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  std::filesystem::remove(path.string() + "-wal", ignored);
  std::filesystem::remove(path.string() + "-shm", ignored);
}

} // namespace
} // namespace manyforge::roboplan_backend
