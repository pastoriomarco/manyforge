#include <gtest/gtest.h>

#include <cstdint>
#include <manyforge_core/resource_addressed_execution_kernel.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace manyforge::core;

class FakeParticipantEngine final : public IResolvedParticipantExecutionEngine {
 public:
  FakeParticipantEngine() {
    gripper_status.state = GripperCommandState::kExecuting;
    gripper_status.gripper_id = "gripper.target";
    gripper_status.command_id = 73U;
    gripper_status.generation = 9U;
    gripper_status.revision = 1U;
    gripper_status.progress = 0.25;
  }

  Result<std::uint64_t> execute(const ExecuteRequestDTO& request) override {
    ++execute_calls;
    last_execute = request;
    if (request.initial_runtime_scale.has_value()) {
      const auto initialized = set_runtime_scale(*request.initial_runtime_scale);
      if (!initialized.ok()) {
        return Result<std::uint64_t>::Err(initialized.error());
      }
    }
    status.state = ExecutionState::kPlanning;
    status.execution_id = next_execution_id;
    return next_execute_result;
  }

  StopResultDTO stop() override {
    ++stop_calls;
    return next_stop_result;
  }
  PauseResultDTO pause() override {
    ++pause_calls;
    if (next_pause_result.accepted) {
      emit_transition(status.state, ExecutionState::kStopping);
    }
    return next_pause_result;
  }
  ResumeResultDTO resume(std::optional<std::uint64_t>) override {
    ++resume_calls;
    if (next_resume_result.accepted) {
      emit_transition(status.state, ExecutionState::kValidating);
    }
    return next_resume_result;
  }
  ResetResultDTO reset(std::optional<std::uint64_t>) override {
    ++reset_calls;
    if (next_reset_result.accepted) {
      emit_transition(status.state, ExecutionState::kIdle);
    }
    return next_reset_result;
  }
  AcknowledgeCompletedResultDTO acknowledge_completed(std::uint64_t,
                                                      std::optional<std::uint64_t>) override {
    ++acknowledge_calls;
    if (next_acknowledge_result.accepted) {
      emit_transition(status.state, ExecutionState::kIdle);
    }
    return next_acknowledge_result;
  }

  Result<ResolvedRuntimeScaleAcceptedDTO> set_runtime_scale(
      const ResolvedRuntimeScaleCommandDTO& command) override {
    ++runtime_scale_calls;
    if (!runtime_scale.supported) {
      return Result<ResolvedRuntimeScaleAcceptedDTO>::Err(ErrorDTO{
          std::string(to_string(ErrorCode::kUnsupportedCapability)),
          "fake participant has no runtime-scale mechanism",
          {{"reason", "runtime_scale_mechanism_unsupported"}}});
    }
    if (!next_runtime_scale_result.ok()) {
      return Result<ResolvedRuntimeScaleAcceptedDTO>::Err(
          next_runtime_scale_result.error());
    }
    last_runtime_scale_command = command;
    runtime_scale.commanded_scale = command.requested_scale;
    runtime_scale.applied_scale = command.requested_scale;
    ++runtime_scale.acknowledgement_sequence;
    runtime_scale.acknowledgement_timestamp_ns = 1;
    return Result<ResolvedRuntimeScaleAcceptedDTO>::Ok(
        ResolvedRuntimeScaleAcceptedDTO{
            command.operation_id,
            command.operation_revision,
            command.participant_id,
            command.controllable_component_id,
            command.command_target_id,
            command.capability_id,
            command.control_domain_id,
            command.control_domain_revision,
            command.control_binding_id,
            command.control_binding_revision,
            command.controller_goal_id,
            command.controller_goal_generation,
            command.controller_topology_id,
            command.topology_revision,
            command.control_route_id,
            command.scaling_domain_id,
            command.coordinated_barrier_id,
            command.resources,
            command.active_control_configuration_id,
            command.active_control_configuration_generation,
            command.requested_runtime_scale_generation,
            command.requested_scale,
            runtime_scale.acknowledgement_sequence,
            runtime_scale.acknowledgement_timestamp_ns});
  }

  Result<ResolvedRuntimeScaleStatusDTO> runtime_scale_status(
      const OperationId& operation_id,
      const ControlBindingId& control_binding_id) const override {
    if (!last_runtime_scale_command.has_value() ||
        last_runtime_scale_command->operation_id != operation_id ||
        last_runtime_scale_command->control_binding_id != control_binding_id ||
        !last_execute.has_value()) {
      return Result<ResolvedRuntimeScaleStatusDTO>::Err(ErrorDTO{
          std::string(to_string(ErrorCode::kMissingReference)),
          "fake runtime-scale status is not initialized",
          {{"reason", "runtime_scale_status_not_initialized"}}});
    }
    const auto& command = *last_runtime_scale_command;
    return Result<ResolvedRuntimeScaleStatusDTO>::Ok(
        ResolvedRuntimeScaleStatusDTO{
            command.operation_id,
            command.operation_revision,
            command.participant_id,
            command.controllable_component_id,
            command.command_target_id,
            command.capability_id,
            command.control_domain_id,
            command.control_domain_revision,
            command.control_binding_id,
            command.control_binding_revision,
            command.controller_goal_id,
            command.controller_goal_generation,
            command.controller_topology_id,
            command.topology_revision,
            command.control_route_id,
            command.scaling_domain_id,
            command.coordinated_barrier_id,
            command.resources,
            command.active_control_configuration_id,
            command.active_control_configuration_generation,
            command.requested_runtime_scale_generation,
            runtime_scale.commanded_scale,
            runtime_scale.applied_scale,
            runtime_scale.virtual_trajectory_time_s,
            last_execute->binding.provider_runtime_epoch,
            runtime_scale.acknowledgement_sequence,
            runtime_scale.acknowledgement_timestamp_ns,
            runtime_scale.error.has_value()
                ? RuntimeScaleApplicationState::kFailed
                : runtime_scale.active
                      ? RuntimeScaleApplicationState::kApplied
                      : RuntimeScaleApplicationState::kPending,
            runtime_scale.error});
  }

  GripperCommandResultDTO command_gripper(const GripperCommandRequestDTO& request) override {
    ++command_calls;
    last_command = request;
    return next_command_result;
  }

  GripperCommandStatusDTO get_gripper_status(const std::string&) const override {
    return gripper_status;
  }

  ExecutionStatus get_status() const override { return status; }

  SubscriptionHandle subscribe_events(EventCallback callback) override {
    const auto id = next_subscription_id++;
    callbacks.emplace(id, std::move(callback));
    return SubscriptionHandle{id};
  }

  void unsubscribe_events(SubscriptionHandle handle) override { callbacks.erase(handle.id); }

  void emit_transition(ExecutionState from, ExecutionState to,
                       std::optional<ErrorDTO> error = std::nullopt) {
    status.state = to;
    runtime_scale.active =
        to == ExecutionState::kExecuting || to == ExecutionState::kPaused;
    status.progress = to == ExecutionState::kCompleted ? 1.0 : 0.4;
    if (error.has_value()) {
      status.last_error = ErrorCode::kExecutionAborted;
    }
    const KernelEvent event{
        KernelEventType::kStateTransition, next_execution_id, 0, from, to, std::move(error), {}};
    const auto snapshot = callbacks;
    for (const auto& [id, callback] : snapshot) {
      (void)id;
      callback(event);
    }
  }

  void emit_error(ErrorDTO error) {
    status.last_error = ErrorCode::kExecutionAborted;
    const KernelEvent event{KernelEventType::kErrorRaised,
                            next_execution_id,
                            0,
                            status.state,
                            status.state,
                            std::move(error),
                            {}};
    const auto snapshot = callbacks;
    for (const auto& [id, callback] : snapshot) {
      (void)id;
      callback(event);
    }
  }

  int execute_calls{};
  int command_calls{};
  int stop_calls{};
  int pause_calls{};
  int resume_calls{};
  int reset_calls{};
  int acknowledge_calls{};
  int runtime_scale_calls{};
  std::uint64_t next_execution_id{41U};
  Result<std::uint64_t> next_execute_result = Result<std::uint64_t>::Ok(41U);
  GripperCommandResultDTO next_command_result{true, 73U, 9U, std::nullopt};
  StopResultDTO next_stop_result{true, std::nullopt};
  PauseResultDTO next_pause_result{true, std::nullopt};
  ResumeResultDTO next_resume_result{true, std::nullopt};
  ResetResultDTO next_reset_result{true, std::nullopt};
  AcknowledgeCompletedResultDTO next_acknowledge_result{true, std::nullopt};
  Result<void> next_runtime_scale_result = Result<void>::Ok();
  RuntimeScaleMechanismStatusDTO runtime_scale{true, false, 1.0, 1.0,
                                                0.0, 1U, 1,
                                                std::nullopt};
  ExecutionStatus status{};
  GripperCommandStatusDTO gripper_status{};
  std::optional<ExecuteRequestDTO> last_execute;
  std::optional<ResolvedRuntimeScaleCommandDTO> last_runtime_scale_command;
  std::optional<GripperCommandRequestDTO> last_command;

 private:
  std::uint64_t next_subscription_id{1U};
  std::map<std::uint64_t, EventCallback> callbacks;
};

[[nodiscard]] RuntimeParticipantBinding make_binding(
    std::string target_role, std::string command_target, std::string capability,
    CoordinateKind kind, std::optional<ProgramCommandSchemaRefDTO> command_schema = std::nullopt,
    bool include_second_angular_resource = false,
    std::string planner_capability_revision =
        "sha256:0f6cb2fda7e1d05f20a5867203b84c0e0427440f4b11657941a907c2e2a23dce") {
  std::vector<std::string> compiled_group_joint_names{
      kind == CoordinateKind::kAngular ? "mf2_joint_1" : "mf2_finger_joint"};
  std::vector<RuntimeResourceBindingDTO> resources{RuntimeResourceBindingDTO{
      ResourceRoleId(kind == CoordinateKind::kAngular ? "arm_joints" : "fingers"),
      ResourceId(kind == CoordinateKind::kAngular ? "robot.joint_1" : "robot.finger"),
      kind == CoordinateKind::kAngular ? "joint_1" : "finger_joint",
      kind == CoordinateKind::kAngular ? "mf2_joint_1" : "mf2_finger_joint", kind}};
  if (kind == CoordinateKind::kAngular && include_second_angular_resource) {
    compiled_group_joint_names.emplace_back("mf2_joint_2");
    resources.push_back(RuntimeResourceBindingDTO{ResourceRoleId("arm_joints"),
                                                  ResourceId("robot.joint_2"), "joint_2",
                                                  "mf2_joint_2", CoordinateKind::kAngular});
  }
  std::vector<RuntimeCapabilityProviderPathDTO> supporting_paths;
  if (kind == CoordinateKind::kAngular) {
    supporting_paths.push_back(RuntimeCapabilityProviderPathDTO{
        CapabilityId("planning.motion.joint_goal"),
        CapabilityDefinitionRevision(std::move(planner_capability_revision)),
        CapabilityProviderBindingId("planner-binding"),
        CapabilityProviderBindingRevision("planner-binding-revision"),
        ProviderInstanceId("planner-provider"), ProviderManifestId("planner-provider-manifest"),
        ProviderManifestRevision("planner-provider-revision"),
        RuntimeEpoch("planner-provider-epoch")});
  }
  RuntimeParticipantBindingDTO value{
      ProgramId("program.pick"),
      ProgramRevision("program-revision"),
      ProgramBindingId("program-binding"),
      RetainedBundleId("bundle"),
      GraphRevision("graph"),
      ComponentRoleId("robot"),
      TargetRoleId(std::move(target_role)),
      ControllableComponentId("robot.1"),
      ComponentTypeId("aggregate.robot"),
      ComponentTypeRevision("aggregate-revision"),
      CommandTargetId(std::move(command_target)),
      CapabilityId(std::move(capability)),
      CapabilityDefinitionRevision("capability-revision"),
      std::move(command_schema),
      {ProgramModelRoleBindingDTO{ModelRoleId("body"), ModelInstanceId("robot.model"),
                                  ModelRevision("model-revision")}},
      {RuntimeModelGroupBindingDTO{
          ModelGroupRoleId("command_group"), ModelInstanceId("robot.model"),
          kind == CoordinateKind::kAngular ? "mf2_arm_group" : "mf2_gripper_group",
          std::move(compiled_group_joint_names)}},
      {ProgramFrameRoleBindingDTO{FrameRoleId("base"), ModelInstanceId("robot.model"),
                                  ModelRevision("model-revision"), "mf2_base"},
       ProgramFrameRoleBindingDTO{FrameRoleId("tcp"), ModelInstanceId("robot.model"),
                                  ModelRevision("model-revision"), "mf2_tcp"}},
      std::move(resources),
      {},
      RuntimeControlPathDTO{
          ControlDomainId(kind == CoordinateKind::kAngular ? "domain.arm" : "domain.gripper"),
          ControlDomainRevision("domain-revision"),
          ControlBindingId(kind == CoordinateKind::kAngular ? "binding.arm" : "binding.gripper"),
          ControlBindingRevision("binding-revision"),
          ControlInterfaceContractId("interface"),
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
          RuntimeEpoch("provider-epoch.1"),
          StateAuthorityId("state.native"),
          StateAuthorityClass::kPredicted,
          StopEvidencePolicyId("stop.native"),
          StopEvidencePolicyRevision("stop-revision"),
          ContainmentGranularity::kResource,
          kind == CoordinateKind::kAngular
              ? std::optional(RuntimeScaleRouteQualificationDTO{
                    ControlRouteId("route.native"),
                    ScalingDomainId("scale.native"),
                    EndpointRoleId("runtime_scale_command"),
                    EndpointRoleId("runtime_scale_state"), std::nullopt,
                    RuntimeScaleAcknowledgementKind::kAppliedState, 4.0,
                    100'000'000})
              : std::nullopt},
      std::nullopt,
      std::move(supporting_paths),
      {}};
  auto sealed = verify_runtime_participant_binding(std::move(value));
  if (!sealed.ok()) {
    throw std::logic_error(sealed.error().message);
  }
  return std::move(sealed.value());
}

[[nodiscard]] OperationIntentDTO motion_operation(std::string request_id = "request.motion.1") {
  return OperationIntentDTO{
      PublicRequestContextDTO{RequestId(std::move(request_id)), RequestOwnerEpoch("kernel.epoch.1"),
                              "test", std::nullopt},
      ProgramId("program.pick"),
      ProgramRevision("program-revision"),
      ProgramBindingId("program-binding"),
      ProgramRunId("run.1"),
      GraphRevision("graph"),
      RetainedBundleId("bundle"),
      std::nullopt,
      {OperationParticipantIntentDTO{
          ComponentRoleId("robot"), TargetRoleId("arm"),
          CapabilityId("execution.trajectory.execute"), std::nullopt,
          MotionParticipantIntentDTO{MoveId("move.1"),
                                     FrameRoleId("base"),
                                     FrameRoleId("tcp"),
                                     TolerancesDTO{0.01, 0.001, 0.01},
                                     SpeedProfileDTO{},
                                     TimeoutsDTO{},
                                     CacheMode::kDefault,
                                     PlanningProfileDTO::kBalanced,
                                     MotionTargetModeDTO::kAbsolute,
                                     std::nullopt,
                                     std::nullopt,
                                     JointGoalDTO{{"joint_1"}, {0.2}},
                                     std::nullopt,
                                     {{"joint_1", JointDynamicsLimitDTO{1.0, 2.0, std::nullopt}}},
                                     {}}}},
      OperationCoordinationPolicy::kIndependent};
}

[[nodiscard]] OperationIntentDTO command_operation() {
  const ProgramCommandSchemaRefDTO schema{
      CommandSchemaId(std::string(kGripperPositionCommandSchemaId)),
      CommandSchemaRevision(std::string(kGripperPositionCommandSchemaRevision))};
  auto operation = motion_operation("request.command.1");
  operation.participants.front() = OperationParticipantIntentDTO{
      ComponentRoleId("robot"), TargetRoleId("gripper"), CapabilityId("tool.gripper.command"),
      std::nullopt,
      CapabilityCommandIntentDTO{
          schema.command_schema_id,
          schema.command_schema_revision,
          GripperPositionCommandValueDTO{LinearPositionDTO{0.02}, LinearEffortDTO{8.0}},
          2.0,
          {CommandDurationOptionDTO{CommandOptionId("command_duration_s"), 0.5}}}};
  return operation;
}

[[nodiscard]] OperationControlRequestDTO control_request(std::string request_id,
                                                         const OperationAcceptedDTO& accepted,
                                                         std::uint64_t expected_revision) {
  return OperationControlRequestDTO{
      PublicRequestContextDTO{RequestId(std::move(request_id)), RequestOwnerEpoch("kernel.epoch.1"),
                              "test", std::nullopt},
      accepted.operation_id, OperationRevision(expected_revision), std::nullopt};
}

[[nodiscard]] StopIntentDTO stop_request(
    std::string request_id, const OperationAcceptedDTO& accepted,
    std::uint64_t expected_revision,
    std::uint64_t expected_stop_generation = 0U) {
  return StopIntentDTO{
      PublicRequestContextDTO{RequestId(std::move(request_id)),
                              RequestOwnerEpoch("kernel.epoch.1"), "test",
                              std::nullopt},
      {StopOperationTargetDTO{accepted.operation_id,
                              OperationRevision(expected_revision),
                              StopGeneration(expected_stop_generation)}},
      std::nullopt};
}

[[nodiscard]] RuntimeScaleRequestDTO runtime_scale_request(
    std::string request_id, const OperationAcceptedDTO& accepted,
    std::uint64_t expected_revision, std::uint64_t expected_generation,
    double scale) {
  return RuntimeScaleRequestDTO{
      PublicRequestContextDTO{RequestId(std::move(request_id)),
                              RequestOwnerEpoch("kernel.epoch.1"), "test",
                              std::nullopt},
      accepted.operation_id, OperationRevision(expected_revision),
      RuntimeScaleGeneration(expected_generation), scale};
}

TEST(ResourceAddressedExecutionKernelTest, ResolvesPortableMotionAndReservesExactResource) {
  FakeClock clock;
  clock.set(1234);
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {{make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
        &engine}},
      clock, ResourceAddressedExecutionKernelConfig{RequestOwnerEpoch("kernel.epoch.1"), 8U});

  const auto result = kernel.execute(motion_operation());
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_EQ(result.value().accepted_at_ns, 1234);
  EXPECT_EQ(engine.execute_calls, 1);
  ASSERT_TRUE(engine.last_execute.has_value());
  EXPECT_EQ(engine.last_execute->binding.controllable_component_id.value(), "robot.1");
  EXPECT_EQ(engine.last_execute->binding.model_group_binding.compiled_group_name, "mf2_arm_group");
  EXPECT_EQ(compiled_frame_name(engine.last_execute->binding.base_frame), "mf2_base");
  EXPECT_EQ(compiled_frame_name(engine.last_execute->binding.tcp_frame), "mf2_tcp");
  const auto& goal = std::get<ResolvedJointGoalDTO>(engine.last_execute->motion);
  EXPECT_EQ(goal.joint_names, std::vector<std::string>{"mf2_joint_1"});
  ASSERT_EQ(engine.last_execute->per_resource_dynamics_limits.size(), 1U);
  EXPECT_EQ(engine.last_execute->per_resource_dynamics_limits.front().resource_id.value(),
            "robot.joint_1");
  EXPECT_EQ(engine.last_execute->per_resource_dynamics_limits.front().compiled_joint_name,
            "mf2_joint_1");

  const auto slots = kernel.resource_snapshot();
  ASSERT_EQ(slots.size(), 1U);
  EXPECT_EQ(slots.front().resource_id.value(), "robot.joint_1");
  EXPECT_EQ(slots.front().state, ResourceSlotState::kReserved);

  const auto state = kernel.get_operation_state(result.value().operation_id);
  ASSERT_TRUE(state.ok()) << state.error().message;
  EXPECT_EQ(state.value().state, OperationStateKind::kPlanning);
  EXPECT_EQ(state.value().participants.front().command_target_id.value(), "arm");
}

TEST(ResourceAddressedExecutionKernelTest, RejectsResourceRoleSpellingUsedAsASourceJointName) {
  FakeClock clock;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {{make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
        &engine}},
      clock, {RequestOwnerEpoch("kernel.epoch.1"), 8U});
  auto intent = motion_operation();
  auto& motion = std::get<MotionParticipantIntentDTO>(intent.participants.front().payload);
  std::get<JointGoalDTO>(motion.motion).joint_names = {"arm_joints"};

  const auto result = kernel.execute(intent);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "joint_goal_source_joint_unresolved");
  EXPECT_EQ(engine.execute_calls, 0);
}

TEST(ResourceAddressedExecutionKernelTest, RejectsStalePlannerCapabilityRevision) {
  FakeClock clock;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {{make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular,
                     std::nullopt, false,
                     "sha256:1111111111111111111111111111111111111111111111111111111111111111"),
        &engine}},
      clock, {RequestOwnerEpoch("kernel.epoch.1"), 8U});

  const auto result = kernel.execute(motion_operation());
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "motion_planner_capability_unresolved");
  EXPECT_EQ(engine.execute_calls, 0);
}

TEST(ResourceAddressedExecutionKernelTest, PreservesAuthoredSourceJointOrderingDuringResolution) {
  FakeClock clock;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {{make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular,
                     std::nullopt, true),
        &engine}},
      clock, {RequestOwnerEpoch("kernel.epoch.1"), 8U});
  auto intent = motion_operation();
  auto& motion = std::get<MotionParticipantIntentDTO>(intent.participants.front().payload);
  motion.motion = JointGoalDTO{{"joint_2", "joint_1"}, {0.4, -0.2}};
  motion.per_joint_dynamics_limits = {{"joint_2", JointDynamicsLimitDTO{0.8, 1.5, std::nullopt}}};

  const auto result = kernel.execute(intent);
  ASSERT_TRUE(result.ok()) << result.error().message;
  ASSERT_TRUE(engine.last_execute.has_value());
  const auto& goal = std::get<ResolvedJointGoalDTO>(engine.last_execute->motion);
  EXPECT_EQ(goal.resources,
            (std::vector<ResourceId>{ResourceId("robot.joint_2"), ResourceId("robot.joint_1")}));
  EXPECT_EQ(goal.joint_names, (std::vector<std::string>{"mf2_joint_2", "mf2_joint_1"}));
  EXPECT_EQ(goal.positions_rad, (std::vector<double>{0.4, -0.2}));
  ASSERT_EQ(engine.last_execute->per_resource_dynamics_limits.size(), 1U);
  EXPECT_EQ(engine.last_execute->per_resource_dynamics_limits.front().resource_id.value(),
            "robot.joint_2");
}

TEST(ResourceAddressedExecutionKernelTest,
     ResolvesAValidSourceJointSubsetWithoutRoleCardinalityCoupling) {
  FakeClock clock;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {{make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular,
                     std::nullopt, true),
        &engine}},
      clock, {RequestOwnerEpoch("kernel.epoch.1"), 8U});
  auto intent = motion_operation();
  auto& motion = std::get<MotionParticipantIntentDTO>(intent.participants.front().payload);
  motion.motion = JointGoalDTO{{"joint_2"}, {0.4}};
  motion.per_joint_dynamics_limits.clear();

  const auto result = kernel.execute(intent);
  ASSERT_TRUE(result.ok()) << result.error().message;
  ASSERT_TRUE(engine.last_execute.has_value());
  const auto& goal = std::get<ResolvedJointGoalDTO>(engine.last_execute->motion);
  EXPECT_EQ(goal.resources, (std::vector<ResourceId>{ResourceId("robot.joint_2")}));
  EXPECT_EQ(goal.joint_names, (std::vector<std::string>{"mf2_joint_2"}));
  EXPECT_EQ(goal.positions_rad, (std::vector<double>{0.4}));
}

TEST(ResourceAddressedExecutionKernelTest,
     ExactReplayDoesNotRepeatSubordinateExecutionAndConflictFails) {
  FakeClock clock;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {{make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
        &engine}},
      clock, {RequestOwnerEpoch("kernel.epoch.1"), 8U});
  const auto intent = motion_operation();

  const auto first = kernel.execute(intent);
  ASSERT_TRUE(first.ok()) << first.error().message;
  const auto replay = kernel.execute(intent);
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  EXPECT_EQ(replay.value().replay_status, RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(replay.value().operation_id, first.value().operation_id);
  EXPECT_EQ(engine.execute_calls, 1);

  auto conflicting = intent;
  std::get<JointGoalDTO>(
      std::get<MotionParticipantIntentDTO>(conflicting.participants.front().payload).motion)
      .positions_rad.front() = 0.3;
  const auto conflict = kernel.execute(conflicting);
  ASSERT_FALSE(conflict.ok());
  EXPECT_EQ(conflict.error().code, "request_id_conflict");
  EXPECT_EQ(engine.execute_calls, 1);
}

TEST(ResourceAddressedExecutionKernelTest, SubordinateEventsAdvanceExactSlotsAndOperationState) {
  FakeClock clock;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {{make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
        &engine}},
      clock, {RequestOwnerEpoch("kernel.epoch.1"), 8U});
  const auto accepted = kernel.execute(motion_operation());
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;

  clock.set(2000);
  engine.emit_transition(ExecutionState::kValidating, ExecutionState::kExecuting);
  EXPECT_EQ(kernel.resource_snapshot().front().state, ResourceSlotState::kDispatched);
  auto executing = kernel.get_operation_state(accepted.value().operation_id);
  ASSERT_TRUE(executing.ok());
  EXPECT_EQ(executing.value().state, OperationStateKind::kExecuting);
  EXPECT_DOUBLE_EQ(executing.value().participants.front().progress, 0.4);

  clock.set(3000);
  engine.emit_transition(ExecutionState::kExecuting, ExecutionState::kCompleted);
  EXPECT_EQ(kernel.resource_snapshot().front().state, ResourceSlotState::kTerminal);
  auto completed = kernel.get_operation_state(accepted.value().operation_id);
  ASSERT_TRUE(completed.ok());
  EXPECT_EQ(completed.value().state, OperationStateKind::kCompleted);
  EXPECT_EQ(completed.value().updated_at_ns, 3000);

  clock.set(4000);
  ASSERT_TRUE(kernel.reconcile_subordinate_state().ok());
  const auto unchanged = kernel.get_operation_state(accepted.value().operation_id);
  ASSERT_TRUE(unchanged.ok());
  EXPECT_EQ(unchanged.value().updated_at_ns, 3000);
}

TEST(ResourceAddressedExecutionKernelTest,
     PreservesTheExactErrorRaisedBeforeASeparateTerminalTransition) {
  FakeClock clock;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {{make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
        &engine}},
      clock, {RequestOwnerEpoch("kernel.epoch.1"), 8U});
  const auto accepted = kernel.execute(motion_operation());
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;

  engine.emit_error(ErrorDTO{"planning_failed", "exact planner failure", {{"reason", "no_path"}}});
  engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kFailed);

  const auto failed = kernel.get_operation_state(accepted.value().operation_id);
  ASSERT_TRUE(failed.ok()) << failed.error().message;
  ASSERT_TRUE(failed.value().participants.front().terminal_error.has_value());
  EXPECT_EQ(failed.value().participants.front().terminal_error->details.at("reason"), "no_path");
  EXPECT_EQ(failed.value().participants.front().terminal_error->message, "exact planner failure");
}

TEST(ResourceAddressedExecutionKernelTest,
     DoesNotAttachARecoveredSubordinateErrorToSuccessfulCompletion) {
  FakeClock clock;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {{make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
        &engine}},
      clock, {RequestOwnerEpoch("kernel.epoch.1"), 8U});
  const auto accepted = kernel.execute(motion_operation());
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;

  engine.emit_error(
      ErrorDTO{"planning_warning", "recoverable planner warning", {{"reason", "recovered"}}});
  engine.emit_transition(ExecutionState::kExecuting, ExecutionState::kCompleted);

  const auto completed = kernel.get_operation_state(accepted.value().operation_id);
  ASSERT_TRUE(completed.ok()) << completed.error().message;
  EXPECT_EQ(completed.value().state, OperationStateKind::kCompleted);
  EXPECT_FALSE(completed.value().participants.front().terminal_error.has_value());
}

TEST(ResourceAddressedExecutionKernelTest, RejectsStaleEpochBeforeResolutionOrSubordinateEffect) {
  FakeClock clock;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {{make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
        &engine}},
      clock, {RequestOwnerEpoch("kernel.epoch.1"), 8U});
  auto intent = motion_operation();
  intent.request_context.request_owner_epoch = RequestOwnerEpoch("kernel.epoch.old");

  const auto result = kernel.execute(intent);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, "stale_request_owner_epoch");
  EXPECT_EQ(engine.execute_calls, 0);
  EXPECT_EQ(kernel.resource_snapshot().front().state, ResourceSlotState::kFree);
}

TEST(ResourceAddressedExecutionKernelTest,
     LinearGripperCommandUsesResolvedTargetAndReconcilesCompletion) {
  FakeClock clock;
  FakeParticipantEngine engine;
  const ProgramCommandSchemaRefDTO schema{
      CommandSchemaId(std::string(kGripperPositionCommandSchemaId)),
      CommandSchemaRevision(std::string(kGripperPositionCommandSchemaRevision))};
  ResourceAddressedExecutionKernel kernel(
      {{make_binding("gripper", "gripper.target", "tool.gripper.command", CoordinateKind::kLinear,
                     schema),
        &engine}},
      clock, {RequestOwnerEpoch("kernel.epoch.1"), 8U});

  const auto accepted = kernel.execute(command_operation());
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  ASSERT_TRUE(engine.last_command.has_value());
  EXPECT_EQ(engine.last_command->gripper_id, "gripper.target");
  EXPECT_EQ(engine.last_command->group_id, "mf2_gripper_group");
  EXPECT_EQ(engine.last_command->joint_name, "mf2_finger_joint");
  EXPECT_DOUBLE_EQ(engine.last_command->target_position, 0.02);
  ASSERT_TRUE(engine.last_command->max_effort.has_value());
  EXPECT_DOUBLE_EQ(*engine.last_command->max_effort, 8.0);
  EXPECT_DOUBLE_EQ(engine.last_command->duration_s, 0.5);
  EXPECT_EQ(kernel.resource_snapshot().front().state, ResourceSlotState::kDispatched);

  engine.gripper_status.state = GripperCommandState::kCompleted;
  engine.gripper_status.progress = 1.0;
  const auto reconciled = kernel.reconcile_subordinate_state();
  ASSERT_TRUE(reconciled.ok()) << reconciled.error().message;
  EXPECT_EQ(kernel.resource_snapshot().front().state, ResourceSlotState::kTerminal);
  const auto state = kernel.get_operation_state(accepted.value().operation_id);
  ASSERT_TRUE(state.ok());
  EXPECT_EQ(state.value().state, OperationStateKind::kCompleted);
}

TEST(ResourceAddressedExecutionKernelTest,
     OperationControlsAdvanceRevisionAndAcknowledgeReleasesResources) {
  FakeClock clock;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {{make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
        &engine}},
      clock, {RequestOwnerEpoch("kernel.epoch.1"), 16U});
  const auto executed = kernel.execute(motion_operation());
  ASSERT_TRUE(executed.ok()) << executed.error().message;
  engine.emit_transition(ExecutionState::kValidating, ExecutionState::kExecuting);

  const auto paused = kernel.pause(control_request("request.pause.1", executed.value(), 1U));
  ASSERT_TRUE(paused.ok()) << paused.error().message;
  EXPECT_EQ(paused.value().operation_revision_after_acceptance, OperationRevision(2U));
  EXPECT_EQ(kernel.resource_snapshot().front().state, ResourceSlotState::kDispatched);
  engine.emit_transition(ExecutionState::kStopping, ExecutionState::kPaused);
  auto paused_state = kernel.get_operation_state(executed.value().operation_id);
  ASSERT_TRUE(paused_state.ok());
  EXPECT_EQ(paused_state.value().state, OperationStateKind::kPaused);
  EXPECT_EQ(paused_state.value().operation_revision, OperationRevision(2U));

  const auto resumed = kernel.resume(control_request("request.resume.1", executed.value(), 2U));
  ASSERT_TRUE(resumed.ok()) << resumed.error().message;
  EXPECT_EQ(resumed.value().operation_revision_after_acceptance, OperationRevision(3U));
  engine.emit_transition(ExecutionState::kValidating, ExecutionState::kExecuting);
  EXPECT_EQ(kernel.resource_snapshot().front().state, ResourceSlotState::kDispatched);
  engine.emit_transition(ExecutionState::kExecuting, ExecutionState::kCompleted);

  const auto acknowledged =
      kernel.acknowledge_completed(control_request("request.ack.1", executed.value(), 3U));
  ASSERT_TRUE(acknowledged.ok()) << acknowledged.error().message;
  EXPECT_EQ(acknowledged.value().operation_revision_after_acceptance, OperationRevision(4U));
  EXPECT_EQ(engine.pause_calls, 1);
  EXPECT_EQ(engine.resume_calls, 1);
  EXPECT_EQ(engine.acknowledge_calls, 1);
  EXPECT_EQ(kernel.resource_snapshot().front().state, ResourceSlotState::kFree);
  EXPECT_FALSE(kernel.get_operation_state(executed.value().operation_id).ok());

  const auto replay =
      kernel.acknowledge_completed(control_request("request.ack.1", executed.value(), 3U));
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  EXPECT_EQ(replay.value().replay_status, RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(engine.acknowledge_calls, 1);
}

TEST(ResourceAddressedExecutionKernelTest,
     ResetSettlesFailedOperationAndRejectsStaleRevisionWithoutEffect) {
  FakeClock clock;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {{make_binding("arm", "arm", "execution.trajectory.execute", CoordinateKind::kAngular),
        &engine}},
      clock, {RequestOwnerEpoch("kernel.epoch.1"), 16U});
  const auto executed = kernel.execute(motion_operation());
  ASSERT_TRUE(executed.ok()) << executed.error().message;
  engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kFailed,
                         ErrorDTO{"planning_failed", "no path", {{"reason", "no_path"}}});

  const auto stale = kernel.reset(control_request("request.reset.stale", executed.value(), 2U));
  ASSERT_FALSE(stale.ok());
  EXPECT_EQ(stale.error().details.at("reason"), "operation_revision_stale");
  EXPECT_EQ(engine.reset_calls, 0);

  const auto reset = kernel.reset(control_request("request.reset.1", executed.value(), 1U));
  ASSERT_TRUE(reset.ok()) << reset.error().message;
  EXPECT_EQ(engine.reset_calls, 1);
  EXPECT_EQ(kernel.resource_snapshot().front().state, ResourceSlotState::kFree);
}

TEST(ResourceAddressedExecutionKernelTest,
     InitializesScalingAtomicallyWithTheExactResolvedExecutionBinding) {
  FakeClock clock;
  clock.set(1234);
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {{make_binding("arm", "arm", "execution.trajectory.execute",
                     CoordinateKind::kAngular),
        &engine}},
      clock, {RequestOwnerEpoch("kernel.epoch.1"), 16U});

  const auto executed = kernel.execute(motion_operation());
  ASSERT_TRUE(executed.ok()) << executed.error().message;
  ASSERT_TRUE(engine.last_execute.has_value());
  ASSERT_TRUE(engine.last_execute->initial_runtime_scale.has_value());
  ASSERT_TRUE(engine.last_runtime_scale_command.has_value());
  EXPECT_EQ(engine.runtime_scale_calls, 1);
  const auto& exact = *engine.last_runtime_scale_command;
  EXPECT_EQ(exact.operation_id, executed.value().operation_id);
  EXPECT_EQ(exact.participant_id,
            executed.value().participant_assignments.front().participant_id);
  EXPECT_EQ(exact.controller_goal_id,
            engine.last_execute->binding.controller_goal_id);
  EXPECT_EQ(exact.control_binding_id,
            engine.last_execute->binding.control_binding_id);
  EXPECT_EQ(exact.control_route_id,
            engine.last_execute->binding.control_route_id);
  EXPECT_EQ(exact.resources, engine.last_execute->binding.resources);
  EXPECT_EQ(exact.expected_runtime_scale_generation,
            RuntimeScaleGeneration(0U));
  EXPECT_EQ(exact.requested_runtime_scale_generation,
            RuntimeScaleGeneration(0U));
  EXPECT_DOUBLE_EQ(exact.requested_scale, 1.0);

  const auto state = kernel.get_operation_state(executed.value().operation_id);
  ASSERT_TRUE(state.ok()) << state.error().message;
  ASSERT_EQ(state.value().participants.size(), 1U);
  const auto& participant = state.value().participants.front();
  EXPECT_EQ(participant.controller_goal_id,
            engine.last_execute->binding.controller_goal_id);
  EXPECT_EQ(participant.resources, engine.last_execute->binding.resources);
  const auto slots = kernel.resource_snapshot();
  ASSERT_EQ(slots.size(), 1U);
  ASSERT_TRUE(slots.front().owner.has_value());
  EXPECT_EQ(slots.front().owner->operation_id, executed.value().operation_id);
  EXPECT_EQ(slots.front().owner->participant_id, participant.participant_id);
  EXPECT_EQ(slots.front().owner->control_binding_id,
            participant.control_binding_id);
}

TEST(ResourceAddressedExecutionKernelTest,
     RuntimeScaleIsGenerationFencedReplaySafeAndZeroIsNotStop) {
  FakeClock clock;
  clock.set(10);
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {{make_binding("arm", "arm", "execution.trajectory.execute",
                     CoordinateKind::kAngular),
        &engine}},
      clock, {RequestOwnerEpoch("kernel.epoch.1"), 32U});
  const auto executed = kernel.execute(motion_operation());
  ASSERT_TRUE(executed.ok()) << executed.error().message;
  engine.emit_transition(ExecutionState::kValidating,
                         ExecutionState::kExecuting);

  const auto first_request = runtime_scale_request(
      "request.scale.1", executed.value(), 1U, 0U, 0.25);
  const auto first = kernel.set_runtime_scale(first_request);
  ASSERT_TRUE(first.ok()) << first.error().message;
  EXPECT_EQ(first.value().runtime_scale_generation,
            RuntimeScaleGeneration(1U));
  EXPECT_EQ(engine.runtime_scale_calls, 2);
  ASSERT_TRUE(engine.last_runtime_scale_command.has_value());
  EXPECT_EQ(engine.last_runtime_scale_command->operation_revision,
            OperationRevision(1U));
  EXPECT_EQ(engine.last_runtime_scale_command
                ->requested_runtime_scale_generation,
            RuntimeScaleGeneration(1U));
  EXPECT_DOUBLE_EQ(engine.last_runtime_scale_command->requested_scale, 0.25);

  const auto replay = kernel.set_runtime_scale(first_request);
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  EXPECT_EQ(replay.value().replay_status,
            RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(engine.runtime_scale_calls, 2);

  auto conflict = first_request;
  conflict.scale = 0.5;
  const auto conflicting = kernel.set_runtime_scale(conflict);
  ASSERT_FALSE(conflicting.ok());
  EXPECT_EQ(conflicting.error().code, "request_id_conflict");
  EXPECT_EQ(engine.runtime_scale_calls, 2);

  const auto stale = kernel.set_runtime_scale(runtime_scale_request(
      "request.scale.stale", executed.value(), 1U, 0U, 0.5));
  ASSERT_FALSE(stale.ok());
  EXPECT_EQ(stale.error().details.at("reason"),
            "runtime_scale_generation_stale");
  EXPECT_EQ(engine.runtime_scale_calls, 2);

  const auto held = kernel.set_runtime_scale(runtime_scale_request(
      "request.scale.hold", executed.value(), 1U, 1U, 0.0));
  ASSERT_TRUE(held.ok()) << held.error().message;
  EXPECT_EQ(held.value().runtime_scale_generation,
            RuntimeScaleGeneration(2U));
  EXPECT_EQ(engine.stop_calls, 0);
  const auto state = kernel.get_operation_state(executed.value().operation_id);
  ASSERT_TRUE(state.ok());
  EXPECT_EQ(state.value().state, OperationStateKind::kExecuting);
  const auto status = kernel.get_runtime_scale(executed.value().operation_id);
  ASSERT_TRUE(status.ok()) << status.error().message;
  EXPECT_EQ(status.value().state, RuntimeScaleApplicationState::kApplied);
  EXPECT_DOUBLE_EQ(status.value().requested_scale, 0.0);
  EXPECT_DOUBLE_EQ(status.value().effective_scale, 0.0);
  ASSERT_EQ(status.value().participants.size(), 1U);
  EXPECT_TRUE(status.value().participants.front().supported);
  EXPECT_EQ(status.value().participants.front().accepted_generation,
            RuntimeScaleGeneration(2U));
}

TEST(ResourceAddressedExecutionKernelTest,
     ScopedStopAdvancesOwnerAtomicallyAndNeverRepeatsItsEffect) {
  FakeClock clock;
  clock.set(100);
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {{make_binding("arm", "arm", "execution.trajectory.execute",
                     CoordinateKind::kAngular),
        &engine}},
      clock, {RequestOwnerEpoch("kernel.epoch.1"), 32U});
  const auto executed = kernel.execute(motion_operation());
  ASSERT_TRUE(executed.ok()) << executed.error().message;
  engine.emit_transition(ExecutionState::kValidating,
                         ExecutionState::kExecuting);

  const auto request = stop_request("request.stop.1", executed.value(), 1U);
  const auto stopped = kernel.stop(request);
  ASSERT_TRUE(stopped.ok()) << stopped.error().message;
  EXPECT_EQ(stopped.value().stop_generation, StopGeneration(1U));
  EXPECT_EQ(stopped.value().operation_revision_after_acceptance,
            OperationRevision(2U));
  EXPECT_EQ(engine.stop_calls, 1);
  const auto slots = kernel.resource_snapshot();
  ASSERT_EQ(slots.size(), 1U);
  EXPECT_EQ(slots.front().state, ResourceSlotState::kStopping);
  ASSERT_TRUE(slots.front().owner.has_value());
  EXPECT_EQ(slots.front().owner->operation_revision, OperationRevision(2U));
  const auto state = kernel.get_operation_state(executed.value().operation_id);
  ASSERT_TRUE(state.ok());
  EXPECT_EQ(state.value().state, OperationStateKind::kStopping);
  EXPECT_EQ(state.value().operation_revision, OperationRevision(2U));
  EXPECT_EQ(state.value().active_stop_id, stopped.value().stop_id);
  EXPECT_EQ(state.value().active_stop_generation, StopGeneration(1U));

  const auto replay = kernel.stop(request);
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  EXPECT_EQ(replay.value().replay_status,
            RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(replay.value().stop_id, stopped.value().stop_id);
  EXPECT_EQ(engine.stop_calls, 1);

  const auto joined = kernel.stop(
      stop_request("request.stop.join", executed.value(), 2U, 1U));
  ASSERT_TRUE(joined.ok()) << joined.error().message;
  EXPECT_EQ(joined.value().stop_id, stopped.value().stop_id);
  EXPECT_EQ(joined.value().stop_generation, StopGeneration(1U));
  EXPECT_EQ(engine.stop_calls, 1);

  const auto stale = kernel.stop(
      stop_request("request.stop.stale", executed.value(), 1U, 0U));
  ASSERT_FALSE(stale.ok());
  EXPECT_EQ(stale.error().details.at("reason"), "operation_revision_stale");
  EXPECT_EQ(engine.stop_calls, 1);

  const auto scaling_after_stop = kernel.set_runtime_scale(
      runtime_scale_request("request.scale.after-stop", executed.value(), 2U,
                            0U, 0.5));
  ASSERT_FALSE(scaling_after_stop.ok());
  EXPECT_EQ(scaling_after_stop.error().details.at("reason"),
            "runtime_scale_state_invalid");
  EXPECT_EQ(engine.runtime_scale_calls, 1);
}

TEST(ResourceAddressedExecutionKernelTest,
     AcceptedStopSurfacesSubordinateRejectionAsInterventionRequired) {
  FakeClock clock;
  FakeParticipantEngine engine;
  engine.next_stop_result = StopResultDTO{
      false,
      ErrorDTO{std::string(to_string(ErrorCode::kExecutionRejected)),
               "controller rejected Stop",
               {{"reason", "controller_stop_rejected"}}}};
  ResourceAddressedExecutionKernel kernel(
      {{make_binding("arm", "arm", "execution.trajectory.execute",
                     CoordinateKind::kAngular),
        &engine}},
      clock, {RequestOwnerEpoch("kernel.epoch.1"), 16U});
  const auto executed = kernel.execute(motion_operation());
  ASSERT_TRUE(executed.ok()) << executed.error().message;
  engine.emit_transition(ExecutionState::kValidating,
                         ExecutionState::kExecuting);

  const auto accepted =
      kernel.stop(stop_request("request.stop.reject", executed.value(), 1U));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  const auto state = kernel.get_operation_state(executed.value().operation_id);
  ASSERT_TRUE(state.ok());
  EXPECT_EQ(state.value().state, OperationStateKind::kInterventionRequired);
  ASSERT_TRUE(state.value().participants.front().terminal_error.has_value());
  EXPECT_EQ(state.value().participants.front().terminal_error->details.at(
                "reason"),
            "controller_stop_rejected");
}

TEST(ResourceAddressedExecutionKernelTest,
     QualifiedScaleFailureIsVisibleAndUnsupportedMechanismsFailAdmission) {
  FakeClock clock;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {{make_binding("arm", "arm", "execution.trajectory.execute",
                     CoordinateKind::kAngular),
        &engine}},
      clock, {RequestOwnerEpoch("kernel.epoch.1"), 16U});
  const auto executed = kernel.execute(motion_operation());
  ASSERT_TRUE(executed.ok()) << executed.error().message;
  engine.emit_transition(ExecutionState::kValidating,
                         ExecutionState::kExecuting);
  engine.next_runtime_scale_result = Result<void>::Err(ErrorDTO{
      std::string(to_string(ErrorCode::kExecutionRejected)),
      "scale handoff failed", {{"reason", "scale_handoff_failed"}}});

  const auto accepted = kernel.set_runtime_scale(runtime_scale_request(
      "request.scale.failure", executed.value(), 1U, 0U, 0.5));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  const auto failed = kernel.get_runtime_scale(executed.value().operation_id);
  ASSERT_TRUE(failed.ok());
  EXPECT_EQ(failed.value().state, RuntimeScaleApplicationState::kFailed);
  ASSERT_TRUE(failed.value().participants.front().error.has_value());
  EXPECT_EQ(failed.value().participants.front().error->details.at("reason"),
            "scale_handoff_failed");

  FakeParticipantEngine unsupported_engine;
  unsupported_engine.runtime_scale.supported = false;
  ResourceAddressedExecutionKernel unsupported_kernel(
      {{make_binding("arm", "arm", "execution.trajectory.execute",
                     CoordinateKind::kAngular),
        &unsupported_engine}},
      clock, {RequestOwnerEpoch("kernel.epoch.1"), 16U});
  auto unsupported_intent = motion_operation("request.motion.unsupported");
  const auto unsupported = unsupported_kernel.execute(unsupported_intent);
  ASSERT_FALSE(unsupported.ok());
  EXPECT_EQ(unsupported.error().details.at("reason"),
            "runtime_scale_mechanism_unsupported");
  EXPECT_EQ(unsupported_engine.execute_calls, 1);
  EXPECT_EQ(unsupported_kernel.resource_snapshot().front().state,
            ResourceSlotState::kFree);
}

TEST(ResourceAddressedExecutionKernelTest,
     DirectControlSpanIsRetainedAndOversizedInputFailsBeforeEffect) {
  FakeClock clock;
  FakeParticipantEngine engine;
  ResourceAddressedExecutionKernel kernel(
      {{make_binding("arm", "arm", "execution.trajectory.execute",
                     CoordinateKind::kAngular),
        &engine}},
      clock, {RequestOwnerEpoch("kernel.epoch.1"), 16U});
  auto direct = motion_operation("request.direct.1");
  direct.participants.front().action_span = ActionSpanDTO{
      ActionSource::kDirectControl, "direct/session.1", "run.1", "", "",
      ""};
  const auto accepted = kernel.execute(direct);
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  ASSERT_TRUE(engine.last_execute.has_value());
  ASSERT_TRUE(engine.last_execute->action_span.has_value());
  EXPECT_EQ(engine.last_execute->action_span->action_source,
            ActionSource::kDirectControl);
  EXPECT_EQ(engine.last_execute->action_span->action_span_id,
            "direct/session.1");

  auto oversized = motion_operation("request.direct.oversized");
  oversized.participants.front().action_span = ActionSpanDTO{
      ActionSource::kDirectControl,
      std::string(kMaxActionSpanFieldBytes + 1U, 'x'), "run.2", "", "",
      ""};
  const auto rejected = kernel.execute(oversized);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().details.at("reason"),
            "operation_action_span_invalid");
  EXPECT_EQ(engine.execute_calls, 1);
}

}  // namespace
