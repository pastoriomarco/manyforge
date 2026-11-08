#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <future>
#include <atomic>
#include <iomanip>
#include <sstream>
#include <manyforge_core/motion_interrupt/stop_profile_mapper.hpp>
#include <manyforge_core/participant_stop_stage_helper.hpp>
#include <manyforge_core/testing/fake_execution_port.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/trajectory/trajectory_sampler.hpp>
#include <memory>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

// Core Stop-stage helper over the fake execution port (the native and ROS
// Stop policy cases live here). Time and port facts are
// scripted through the injected waiter; no sleeps.

namespace manyforge::core {
namespace {

using testing::FakeExecutionPort;

constexpr Duration kTick = 10'000'000;  // one controller_state sample per tick
const ControllerGoalId kGoalId("goal.direct");
const ExecutionGoalKey kOriginal{kGoalId, ControllerGoalGeneration(1U)};
const ExecutionGoalKey kSecond{kGoalId, ControllerGoalGeneration(2U)};
const ExecutionGoalKey kThird{kGoalId, ControllerGoalGeneration(3U)};
const RuntimeEpoch kEpoch("runtime-epoch.1");

std::shared_ptr<GoalCancelAttempt> TestCancelAttempt() {
  auto attempt = std::make_shared<GoalCancelAttempt>();
  attempt->execution_id = 1U;
  return attempt;
}

enum class Nominal { kControlled, kGoalCancel };

ResolvedStopRuntimeTargetDTO Target() {
  return ResolvedStopRuntimeTargetDTO{ProviderInstanceId("provider.jtc"),
                                      ProviderManifestId("provider-manifest.jtc"),
                                      ProviderManifestRevision("provider-manifest-revision"),
                                      RuntimeUnitId("runtime.jtc"),
                                      ProductId("product.jtc"),
                                      ProductContractId("product.contract.jtc"),
                                      ProductContractRevision("product-contract-revision"),
                                      ContentDigest("runtime-target-digest"),
                                      kEpoch,
                                      kEpoch,
                                      std::nullopt,
                                      std::nullopt,
                                      std::nullopt,
                                      std::nullopt,
                                      std::nullopt,
                                      std::nullopt,
                                      std::nullopt,
                                      std::nullopt};
}

ResolvedStopEvidenceRequirementDTO Evidence(const char* item, StopEvidenceRequirementKind kind) {
  const bool rest = kind == StopEvidenceRequirementKind::kMotionRest;
  const auto source =
      rest ? StopEvidenceSourceKind::kBindingStateAuthority : StopEvidenceSourceKind::kControllerGoalStatus;
  return ResolvedStopEvidenceRequirementDTO{
      StopEvidenceRequirementDTO{
          StopEvidenceItemId(item),
          kind,
          std::nullopt,
          std::nullopt,
          StopEvidenceSourceRequirementDTO{source, 100'000'000, std::nullopt, {}},
          StopEvidenceObservationDTO{StopEvidenceObservationKind::kPostCommandSample,
                                     std::nullopt}},
      ResolvedStopEvidenceSourceDTO{
          source, Target(),
          rest ? std::optional(StateAuthorityId("state.commanded")) : std::nullopt,
          rest ? std::optional(StateAuthorityClass::kCommanded) : std::nullopt, std::nullopt}};
}

ResolvedStopEscalationStageDTO StageRow(std::uint64_t index, const char* id,
                                        StopEscalationStageKind kind,
                                        StopContainmentCommandDTO command,
                                        ResolvedStopEvidenceRequirementDTO evidence) {
  return ResolvedStopEscalationStageDTO{
      index,
      StopEscalationStageId(id),
      kind,
      ContainmentGranularity::kResource,
      std::move(command),
      std::nullopt,
      ResolvedStopCommandQualificationDTO{},
      Target(),
      ResolvedStopResourceContainmentScopeDTO{ActivationSetId("activation.jtc"),
                                              {ResourceId("robot.joint.1")}},
      StopEvidenceConjunction::kAllOf,
      {std::move(evidence)}};
}

/// Contract parameters of the controlled stage (the ROS fixture's shape).
JsonObjectDTO Parameters(std::string mode, bool natural = true,
                         std::optional<std::string> substitution = std::nullopt,
                         double deceleration = 4.0) {
  JsonObjectDTO row{
      {"resource_id", JsonValueDTO{std::string("robot.joint.1")}},
      {"settle_velocity_threshold",
       JsonValueDTO{JsonObjectDTO{{"coordinate_kind", JsonValueDTO{std::string("angular_rad_s")}},
                                  {"value_rad_s", JsonValueDTO{0.01}}}}}};
  JsonObjectDTO parameters{{"stop_mode", JsonValueDTO{mode}},
                           {"per_resource_limits", JsonValueDTO{JsonValueDTO::Array{}}}};
  if (mode != "hard_deceleration") {
    row.emplace("max_deceleration",
                JsonValueDTO{JsonObjectDTO{
                    {"coordinate_kind", JsonValueDTO{std::string("angular_rad_s2")}},
                    {"value_rad_s2", JsonValueDTO{deceleration}}}});
    parameters.emplace("jerk_limits_enabled", JsonValueDTO{false});
    parameters.emplace("deceleration_time_s", JsonValueDTO{1.0});
    parameters.emplace("allow_natural_completion_on_short_path", JsonValueDTO{natural});
    if (substitution) parameters.emplace("recoverable_substitution_mode", JsonValueDTO{*substitution});
  }
  std::get<JsonValueDTO::Array>(parameters.at("per_resource_limits").value)
      .push_back(JsonValueDTO{std::move(row)});
  return parameters;
}

JsonObjectDTO FallbackParameters(std::string primary, std::vector<std::string> fallbacks,
                                 double deceleration = 4.0) {
  auto parameters = Parameters(std::move(primary), false, std::nullopt, deceleration);
  JsonValueDTO::Array modes;
  for (auto& mode : fallbacks) modes.push_back(JsonValueDTO{std::move(mode)});
  parameters.emplace("fallback_stop_modes", JsonValueDTO{std::move(modes)});
  return parameters;
}

/// One participant Stop plan: a nominal stage followed by the software-hard
/// stage.
ResolvedParticipantStopStageDTO Stage(std::size_t index, Nominal nominal = Nominal::kControlled,
                                      JsonObjectDTO parameters = Parameters("soft_on_path")) {
  const ProviderContractRefDTO contract{
      ProviderContractRoleId("stop_contract"),
      SchemaId(std::string(kMotionControlledStopContractSchemaId)),
      SchemaRevision(std::string(kMotionControlledStopContractSchemaRevision))};
  std::vector<PreparedStopStageCommandDTO> commands;
  if (nominal == Nominal::kControlled) {
    auto row = StageRow(0U, "stage.controlled", StopEscalationStageKind::kNominal,
                        {StopContainmentCommandKind::kMotionControlledStop, contract, std::nullopt},
                        Evidence("controlled.rest", StopEvidenceRequirementKind::kMotionRest));
    row.default_contract_parameters = parameters;
    commands.push_back({std::move(row), parameters, StopStageParameterSourceDTO::kDeploymentDefault});
  } else {
    commands.push_back(
        {StageRow(0U, "stage.cancel", StopEscalationStageKind::kNominal,
                  {StopContainmentCommandKind::kControllerGoalCancel, std::nullopt, std::nullopt},
                  Evidence("goal.terminal", StopEvidenceRequirementKind::kControllerGoalTerminal)),
         std::nullopt, std::nullopt});
  }
  commands.push_back(
      {StageRow(1U, "stage.software_hard", StopEscalationStageKind::kSoftwareHard,
                {StopContainmentCommandKind::kRegistered, std::nullopt,
                 RegisteredStopCommandKindId("manyforge.stop.software_hard")},
                Evidence("hard.rest", StopEvidenceRequirementKind::kMotionRest)),
       std::nullopt, std::nullopt});
  const OperationId operation("operation.direct");
  const OperationParticipantId participant("participant.direct");
  return ResolvedParticipantStopStageDTO{
      StopId("stop.direct"),
      StopGeneration(1U),
      operation,
      OperationRevision(1U),
      participant,
      kGoalId,
      ControllerGoalGeneration(1U),
      ControllableComponentId("robot.1"),
      CommandTargetId("arm.target"),
      CapabilityId("execution.trajectory.execute"),
      ControlDomainId("domain.arm"),
      ControlDomainRevision("domain-revision"),
      ControlBindingId("binding.arm"),
      ControlBindingRevision("binding-revision"),
      ActiveControlConfigurationId("active.jtc"),
      ActiveControlConfigurationGeneration(1U),
      ControllerTopologyId("topology.jtc"),
      TopologyRevision("topology-revision"),
      ControlRouteId("route.jtc"),
      std::nullopt,
      std::nullopt,
      kEpoch,
      {ResourceId("robot.joint.1")},
      ContainmentGranularity::kResource,
      StopEvidencePolicyId("stop.jtc"),
      StopEvidencePolicyRevision("stop-revision"),
      PreparedParticipantStopPlanDTO{
          ContentDigest("plan.digest"),
          ResolvedStopBasePlanId("base.plan"),
          operation,
          OperationRevision(1U),
          participant,
          ProgramId("program.direct"),
          ProgramRevision("program-revision"),
          ProgramBindingId("program-binding"),
          ProgramRunId("program.run"),
          std::nullopt,
          ParticipantStopProfilePurposeDTO::kOrdinary,
          std::nullopt,
          std::move(commands)},
      index,
      ContentDigest("scope.digest")};
}

/// The Kernel's per-robot Stop-time values `elapsed_ns` after the Stop
/// request: a controlled stage carries its own profile deadline; a hard
/// or cancel stage carries none (the lane bounds it).
RemainingStopBudgetDTO Budget(const ResolvedParticipantStopStageDTO& stage,
                              std::int64_t elapsed_ns = 0) {
  RemainingStopBudgetDTO budget{stage.stop_id, stage.stop_generation, elapsed_ns, std::nullopt};
  const auto& command = stage.participant_stop_plan.stage_commands.at(stage.stage_index);
  if (command.resolved_stage.containment_command.command_kind ==
          StopContainmentCommandKind::kMotionControlledStop &&
      command.validated_contract_parameters.has_value()) {
    const auto deadline =
        controlled_stop_deadline_from_contract_parameters(*command.validated_contract_parameters);
    if (deadline.has_value()) budget.remaining_controlled_deadline_ns = *deadline - elapsed_ns;
  }
  return budget;
}

/// One joint, 0 -> 2 rad over 2 path seconds (1 rad per path second).
TrajectoryDTO Original() {
  TrajectoryDTO trajectory;
  trajectory.resources = {ResourceId("robot.joint.1")};
  trajectory.joint_names = {"joint_1"};
  for (int index = 0; index <= 8; ++index) {
    const double t = 0.25 * index;
    trajectory.waypoints.push_back({t, {t}, {1.0}, {0.0}});
  }
  trajectory.runtime_scale_bounds = TrajectoryRuntimeScaleBoundsDTO{4.0, 16.0};
  return trajectory;
}

StopStageDispatchedGoalDTO Goal(const ResolvedParticipantStopStageDTO& stage) {
  return StopStageDispatchedGoalDTO{
      kOriginal,
      Original(),
      stage.operation_id,
      stage.operation_revision,
      stage.participant_id,
      stage.controllable_component_id,
      stage.command_target_id,
      stage.capability_id,
      stage.control_domain_id,
      stage.control_domain_revision,
      stage.control_binding_id,
      stage.control_binding_revision,
      stage.controller_topology_id,
      stage.topology_revision,
      stage.control_route_id,
      stage.active_control_configuration_id,
      stage.active_control_configuration_generation,
      PreparedProviderRuntimeOccurrenceDTO{ProviderInstanceId("provider.jtc"),
                                           ProviderManifestId("provider-manifest.jtc"),
                                           ProviderManifestRevision("provider-manifest-revision"),
                                           RuntimeUnitId("runtime.jtc"), kEpoch},
      stage.resources,
      stage.stop_evidence_policy_id,
      stage.stop_evidence_policy_revision,
      {stage.participant_stop_plan}};
}

std::vector<FakeExecutionPort::ReplaceCall> Replaces(const FakeExecutionPort& port) {
  std::vector<FakeExecutionPort::ReplaceCall> calls;
  for (const auto& command : port.commands()) {
    if (const auto* call = std::get_if<FakeExecutionPort::ReplaceCall>(&command)) {
      calls.push_back(*call);
    }
  }
  return calls;
}

std::vector<ExecutionGoalKey> Cancels(const FakeExecutionPort& port) {
  std::vector<ExecutionGoalKey> keys;
  for (const auto& command : port.commands()) {
    if (const auto* call = std::get_if<FakeExecutionPort::CancelCall>(&command)) {
      keys.push_back(call->key);
    }
  }
  return keys;
}

std::vector<double> Scales(const FakeExecutionPort& port) {
  std::vector<double> values;
  for (const auto& command : port.commands()) {
    if (const auto* call = std::get_if<FakeExecutionPort::SetScaleCall>(&command)) {
      values.push_back(call->value);
    }
  }
  return values;
}

const JsonObjectDTO& EvidenceObject(const ParticipantStopStageResultDTO& result) {
  return std::get<JsonObjectDTO>(result.evidence_items.front().evidence->value);
}

std::string Text(const JsonObjectDTO& object, const char* key) {
  return std::get<std::string>(object.at(key).value);
}

/// Changes only the returned copy of a goal read, never the stored DTO.
class GoalReadObservingPort final : public IExecutionPort {
 public:
  explicit GoalReadObservingPort(FakeExecutionPort& port) : port_(port) {}

  ExecutionPortCommandResultDTO Submit(const ExecutionGoalKey& key,
                                       const TrajectoryDTO& trajectory, std::shared_ptr<ExecutionDispatchToken> token = {}) override {
    return port_.Submit(key, trajectory, std::move(token));
  }
  ExecutionPortCommandResultDTO Replace(
      const ExecutionGoalKey& active, const ExecutionGoalKey& next,
      const TrajectoryDTO& trajectory, std::shared_ptr<ExecutionDispatchToken> token = {}) override {
    return port_.Replace(active, next, trajectory, std::move(token));
  }
  ExecutionPortCommandResultDTO Cancel(const ExecutionGoalKey& key, std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max()) override {
    if (on_cancel) return on_cancel(key);
    return port_.Cancel(key, deadline);
  }
  std::uint64_t ReserveScaleSequence() noexcept override { return port_.ReserveScaleSequence(); }
  bool IsLatestScaleSequence(std::uint64_t sequence) const noexcept override { return port_.IsLatestScaleSequence(sequence); }
  ScaleSnapshot SnapshotScaleAfterFence(std::chrono::steady_clock::time_point deadline) override { return port_.SnapshotScaleAfterFence(deadline); }
  ExecutionPortCommandResultDTO SetScale(double value, std::uint64_t sequence) override { return port_.SetScale(value, sequence); }
  std::optional<ExecutionGoalObservationDTO> ReadGoal(const ExecutionGoalKey& key) const override {
    auto goal = port_.ReadGoal(key);
    if (on_read_goal) on_read_goal(goal);
    return goal;
  }
  std::optional<ExecutionControllerSampleDTO> ReadSample() const override {
    return port_.ReadSample();
  }
  ExecutionReadinessDTO ReadReadiness() const override { return port_.ReadReadiness(); }
  ExecutionTimingDTO ReadTiming() override { return port_.ReadTiming(); }
  ExecutionLaneFactsDTO ReadLaneFacts() const override { return port_.ReadLaneFacts(); }

  std::function<void(std::optional<ExecutionGoalObservationDTO>&)> on_read_goal;
  std::function<ExecutionPortCommandResultDTO(const ExecutionGoalKey&)> on_cancel;

 private:
  FakeExecutionPort& port_;
};

/// Scripted world: a fake clock, the fake port, and per-tick scripts.
class World {
 public:
  explicit World(bool build = true) {
    clock.set(1'000'000'000);
    ExecutionLaneFactsDTO facts;
    facts.joint_names = {"joint_1"};
    facts.supports_scale = true;
    facts.has_measured_velocity = true;
    facts.state_rate_hz = 100.0;
    facts.controller_update_rate_hz = 250.0;
    port.SetLaneFacts(facts);
    config.resources = {RuntimeResourceBindingDTO{ResourceRoleId("joint_1"),
                                                  ResourceId("robot.joint.1"), "source_joint_1",
                                                  "joint_1", CoordinateKind::kAngular}};
    config.trip_authority = StateAuthorityClass::kCommanded;
    config.stop_traj_delta_s = 0.05;
    config.sample_period_s = 0.01;
    config.replacement_start_allowance_s = 0.02;
    config.max_deceleration_on_cancel_rad_s2 = {{"joint_1", 4.0}};
    config.hard_stop_completion_timeout_ns = 1'000'000'000;
    if (build) Build();
  }

  void Build(IExecutionPort* execution_port = nullptr) {
    helper = std::make_unique<ParticipantStopStageHelper>(
        execution_port == nullptr ? port : *execution_port, clock,
        [this](TimePoint deadline, const std::stop_token&) {
          clock.advance(std::max<Duration>(1, std::min(kTick, deadline - clock.now())));
          ++ticks;
          if (on_tick) on_tick();
        },
        [this](auto) -> std::optional<double> {
          ++freezes;
          if (freeze_expired) return std::nullopt;
          return frozen;
        },
        config);
  }

  void Lane(ExecutionGoalTolerancePolicy tolerance, bool supports_scale = true) {
    auto facts = port.ReadLaneFacts();
    facts.goal_tolerance_policy = tolerance;
    facts.supports_scale = supports_scale;
    port.SetLaneFacts(facts);
  }

  /// The dispatched goal is executing with action feedback already received.
  void Dispatch(const ResolvedParticipantStopStageDTO& stage) {
    auto cancel = std::make_shared<GoalCancelAttempt>();
    cancel->execution_id = 1U;
    helper->BindDispatchedGoal(Goal(stage), std::make_shared<ExecutionSampleAssociation>(
                                                ExecutionBindingEpoch(1U), port.ReadSample(),
                                                clock.now()), std::move(cancel));
    Executing(kOriginal);
  }

  void Executing(const ExecutionGoalKey& key) {
    ExecutionGoalObservationDTO goal;
    goal.key = key;
    goal.binding_epoch = ExecutionBindingEpoch(1U);
    goal.phase = ExecutionGoalPhase::kExecuting;
    goal.mechanism_goal_id = MechanismGoalId("uuid-" + std::to_string(key.controller_goal_generation.value()));
    ExecutionGoalFeedbackDTO feedback;
    feedback.source_stamp_ns = clock.now() - 1;
    feedback.local_receipt_stamp_ns = clock.now() - 1;
    goal.latest_feedback = feedback;
    port.SetGoal(goal);
  }

  /// The brake's final point (level-3 finish): the last point of the
  /// newest replacement as sent.
  double BrakeEnd() const {
    const auto replaces = Replaces(port);
    return replaces.back().trajectory.waypoints.back().positions_rad.front();
  }

  /// Positive acknowledgement of a cancel and the goal's CANCELED result.
  void Cancelled(const ExecutionGoalKey& key) {
    port.AcknowledgeCancel(key, clock.now());
    port.FinishGoal(key, ExecutionGoalResult::kCanceled, std::nullopt, clock.now());
  }

  void Finish(const ExecutionGoalKey& key, ExecutionGoalResult result) {
    port.FinishGoal(key, result, std::nullopt, clock.now());
  }

  void Sample(double cursor, double position, double velocity, std::optional<double> factor = 0.5,
              std::optional<double> measured = std::nullopt,
              std::optional<TimePoint> source_stamp = std::nullopt) {
    ExecutionControllerSampleDTO sample;
    sample.source_stamp_ns = source_stamp.value_or(clock.now());
    sample.local_receipt_stamp_ns = clock.now();
    sample.local_receipt_count = ExecutionReceiptCount(++receipts);
    sample.binding_epoch = ExecutionBindingEpoch(1U);
    sample.cursor_s = cursor;
    sample.applied_factor = factor;
    sample.reference_positions_rad = {position};
    sample.reference_velocities_rad_s = {velocity};
    sample.reference_accelerations_rad_s2 = {0.0};
    sample.command_positions_rad = {position};
    sample.measured_positions_rad = {measured.value_or(position)};
    sample.measured_velocities_rad_s = std::vector<double>{velocity * factor.value_or(1.0)};
    sample.error_positions_rad = {0.0};
    port.SetSample(sample);
  }

  std::size_t CommandCount() const { return port.commands().size(); }

  FakeClock clock;
  FakeExecutionPort port;
  StopStageHelperConfig config;
  std::unique_ptr<ParticipantStopStageHelper> helper;
  std::function<void()> on_tick;
  double frozen{0.5};
  bool freeze_expired{false};
  std::size_t ticks{0U};
  std::size_t freezes{0U};
  std::uint64_t receipts{0U};
};

/// Scripts the mechanism executing a Replace: accepted on the next tick,
/// samples restart on the new trajectory, terminal `result` after `ticks`.
void ScriptReplacement(World& world, const ExecutionGoalKey& key, std::size_t ticks,
                       std::optional<ExecutionGoalResult> result) {
  auto count = std::make_shared<std::size_t>(0U);
  world.on_tick = [&world, key, ticks, result, count] {
    const auto replaces = Replaces(world.port);
    if (replaces.empty()) return;
    ++*count;
    if (*count == 1U) {
      world.Executing(key);
      // The accepted replacement preempts the goal it replaced.
      world.port.FinishGoal(replaces.back().active_key, ExecutionGoalResult::kPreemptedBy, key,
                            world.clock.now());
    }
    world.Sample(0.01 * static_cast<double>(*count), 0.5, *count >= ticks ? 0.0 : 0.5);
    if (*count == ticks && result) world.Finish(key, *result);
  };
}

// --- Stage identity, membership and budget -------------------------------------

// A second possibly-sent goal is retained in the same helper record list.
void RecordPendingReplacement(World& world) {
  ASSERT_TRUE(world.helper->ReserveReplacement(kOriginal));
  ExecutionGoalObservationDTO candidate;
  candidate.key = kSecond;
  candidate.binding_epoch = ExecutionBindingEpoch(1U);
  candidate.phase = ExecutionGoalPhase::kPending;
  world.port.SetGoal(std::move(candidate));
  world.helper->RecordReplacement(
      kSecond, Original(),
      std::make_shared<ExecutionSampleAssociation>(ExecutionBindingEpoch(1U),
                                                   world.port.ReadSample(), world.clock.now()),
      0.0, TestCancelAttempt());
}

TEST(ParticipantStopStageHelperTest, ConcurrentAndLateCallersShareExactCancelAttempt) {
  World world(false);
  GoalReadObservingPort observed(world.port);
  world.Build(&observed);
  world.Dispatch(Stage(0U));
  auto attempt = std::make_shared<GoalCancelAttempt>();
  std::promise<void> entered;
  std::promise<void> release;
  const auto released = release.get_future().share();
  std::atomic<unsigned> calls{0};
  observed.on_cancel = [&](const ExecutionGoalKey& key) {
    if (calls.fetch_add(1) == 0) entered.set_value();
    released.wait();
    return world.port.Cancel(key);
  };
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  auto first = std::async(std::launch::async, [&] {
    return world.helper->CancelExactGoal(kOriginal, attempt, deadline);
  });
  entered.get_future().wait();
  auto second = std::async(std::launch::async, [&] {
    return world.helper->CancelExactGoal(kOriginal, attempt, deadline);
  });
  release.set_value();
  const auto first_result = first.get();
  const auto second_result = second.get();
  const auto late_result = world.helper->CancelExactGoal(kOriginal, attempt, deadline);
  EXPECT_EQ(calls.load(), 1U);
  EXPECT_EQ(first_result.dispatch, second_result.dispatch);
  EXPECT_EQ(first_result.dispatch, late_result.dispatch);
}

TEST(ParticipantStopStageHelperTest, PendingSuccessorBlocksNaturalCompletionUntilExactlyRejected) {
  World world;
  world.Dispatch(Stage(0U));
  RecordPendingReplacement(world);
  world.Finish(kOriginal, ExecutionGoalResult::kSucceeded);
  EXPECT_FALSE(world.helper->CanCompleteNaturally(kOriginal));
  EXPECT_FALSE(world.helper->ReserveReplacement(kOriginal));
  world.Finish(kSecond, ExecutionGoalResult::kEndedUnknown);
  EXPECT_FALSE(world.helper->CanCompleteNaturally(kOriginal));
  EXPECT_TRUE(world.helper->HasLiveGoal());
  world.Finish(kSecond, ExecutionGoalResult::kCanceled);
  EXPECT_FALSE(world.helper->CanCompleteNaturally(kOriginal));
  world.Finish(kSecond, ExecutionGoalResult::kRejected);
  EXPECT_TRUE(world.helper->CanCompleteNaturally(kOriginal));
  world.helper->RetireSettledReplacement(kSecond);
  EXPECT_FALSE(world.helper->HasPendingReplacement());
  EXPECT_TRUE(world.helper->ReserveReplacement(kOriginal));
}

TEST(ParticipantStopStageHelperTest, MissingPendingSuccessorRemainsLiveAndIsCancelled) {
  World world(false);
  GoalReadObservingPort observed(world.port);
  world.Build(&observed);
  world.Dispatch(Stage(0U));
  RecordPendingReplacement(world);
  world.Finish(kOriginal, ExecutionGoalResult::kSucceeded);
  observed.on_read_goal = [](auto& goal) {
    if (goal && goal->key == kSecond) goal.reset();
  };
  EXPECT_FALSE(world.helper->CanCompleteNaturally(kOriginal));
  EXPECT_TRUE(world.helper->HasLiveGoal());
  EXPECT_TRUE(world.helper->CancelLiveGoals());
  EXPECT_EQ(Cancels(world.port), std::vector<ExecutionGoalKey>{kSecond});
}

TEST(ParticipantStopStageHelperTest, PendingFaultCancellationRequestsBothKeysNewestFirst) {
  World world;
  world.Dispatch(Stage(0U));
  RecordPendingReplacement(world);
  world.port.QueueCommandResult(
      {ExecutionPortDispatch::kEffectUncertain, std::nullopt, std::nullopt});
  world.port.QueueCommandResult(
      {ExecutionPortDispatch::kNotDispatched, ExecutionPortRefusal::kStaleKey, std::nullopt});
  EXPECT_TRUE(world.helper->CancelLiveGoals());
  EXPECT_EQ(Cancels(world.port), (std::vector<ExecutionGoalKey>{kSecond, kOriginal}));
}

TEST(ParticipantStopStageHelperTest, KnownSupersededTerminalSurvivesNativeRecordEviction) {
  World world;
  world.Dispatch(Stage(0U));
  RecordPendingReplacement(world);
  world.Executing(kSecond);
  world.port.FinishGoal(kOriginal, ExecutionGoalResult::kPreemptedBy, kSecond, world.clock.now());
  EXPECT_TRUE(world.helper->HasLiveGoal());  // retains exact old terminal before eviction
  ExecutionGoalObservationDTO rejected;
  rejected.key = kThird;
  rejected.binding_epoch = ExecutionBindingEpoch(1U);
  rejected.phase = ExecutionGoalPhase::kTerminal;
  rejected.result = ExecutionGoalResult::kRejected;
  world.port.SetGoal(std::move(rejected));
  ASSERT_FALSE(world.port.ReadGoal(kOriginal));
  EXPECT_TRUE(world.helper->CancelLiveGoals());
  EXPECT_EQ(Cancels(world.port), std::vector<ExecutionGoalKey>{kSecond});
}

TEST(ParticipantStopStageHelperTest, PendingCancelThrowStillRequestsOtherRetainedKey) {
  World world(false);
  GoalReadObservingPort observed(world.port);
  world.Build(&observed);
  world.Dispatch(Stage(0U));
  RecordPendingReplacement(world);
  observed.on_cancel = [&](const ExecutionGoalKey& key) {
    const auto result = world.port.Cancel(key);
    if (key == kSecond) throw std::runtime_error("send outcome unknown");
    return result;
  };
  EXPECT_TRUE(world.helper->CancelLiveGoals());
  EXPECT_EQ(Cancels(world.port), (std::vector<ExecutionGoalKey>{kSecond, kOriginal}));
}

TEST(ParticipantStopStageHelperTest, PendingCancelProofCannotHideAnotherUnresolvedKey) {
  for (const bool superseded : {false, true}) {
    SCOPED_TRACE(superseded);
    World world;
    world.config.max_deceleration_on_cancel_rad_s2.clear();
    world.Build();
    const auto stage = Stage(1U);
    world.Dispatch(stage);
    RecordPendingReplacement(world);
    world.Executing(kSecond);
    world.Sample(0.5, 0.5, 1.0);
    world.on_tick = [&] {
      if (world.ticks != 1U) return;
      if (superseded)
        world.port.FinishGoal(kOriginal, ExecutionGoalResult::kPreemptedBy, kSecond,
                              world.clock.now());
      world.port.MarkCancelSent(kSecond, world.clock.now() - 1);
      world.Cancelled(kSecond);
      world.Sample(0.0, 0.5, 0.0, 0.0);
    };
    const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
    EXPECT_EQ(result.outcome, superseded ? ParticipantStopStageOutcomeDTO::kSucceeded
                                         : ParticipantStopStageOutcomeDTO::kFailed);
    EXPECT_EQ(Cancels(world.port), (std::vector<ExecutionGoalKey>{kSecond, kOriginal}));
    EXPECT_LE(world.clock.now() - 1'000'000'000, world.config.hard_stop_completion_timeout_ns);
  }
}

TEST(ParticipantStopStageHelperTest, RejectedPendingSuccessorRecancelsRestoredPredecessorOnce) {
  for (const bool finish : {false, true}) {
    SCOPED_TRACE(finish);
    World world(false);
    world.config.max_deceleration_on_cancel_rad_s2.clear();
    GoalReadObservingPort observed(world.port);
    world.Build(&observed);
    const auto stage = Stage(1U);
    world.Dispatch(stage);
    world.Sample(1.0, 1.0, 1.0, 1.0);
    RecordPendingReplacement(world);
    unsigned old_cancel_count = 0U;
    observed.on_cancel = [&](const ExecutionGoalKey& key) {
      if (key == kSecond) {
        world.port.QueueCommandResult(
            {ExecutionPortDispatch::kEffectUncertain, std::nullopt, std::nullopt});
      } else if (++old_cancel_count == 1U) {
        world.port.QueueCommandResult(
            {ExecutionPortDispatch::kNotDispatched, ExecutionPortRefusal::kStaleKey, std::nullopt});
      }
      const auto result = world.port.Cancel(key);
      if (key == kOriginal && old_cancel_count == 2U && finish) {
        world.port.MarkCancelSent(key, world.clock.now());
        world.port.AcknowledgeCancel(key, world.clock.now());
      }
      return result;
    };
    world.on_tick = [&] {
      if (world.ticks == 1U) {
        world.Finish(kSecond, ExecutionGoalResult::kRejected);
        world.Sample(1.1, 1.1, 1.0, 1.0);
      } else if (finish && old_cancel_count == 2U) {
        world.Finish(kOriginal, ExecutionGoalResult::kCanceled);
        world.Sample(0.0, 1.1, 0.0, 1.0);
      }
    };
    const auto started = world.clock.now();
    const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
    EXPECT_EQ(Cancels(world.port), (std::vector<ExecutionGoalKey>{kSecond, kOriginal, kOriginal}));
    EXPECT_EQ(old_cancel_count, 2U);
    EXPECT_LE(world.clock.now() - started, world.config.hard_stop_completion_timeout_ns);
    EXPECT_EQ(result.outcome, finish ? ParticipantStopStageOutcomeDTO::kSucceeded
                                     : ParticipantStopStageOutcomeDTO::kFailed);
    if (finish) {
      EXPECT_EQ(
          Text(std::get<JsonObjectDTO>(EvidenceObject(result).at("post_command_sample").value),
               "finish_basis"),
          "cancel_reference_at_rest");
    }
  }
}

TEST(ParticipantStopStageHelperTest, RefusesBeforeEffectWithoutExactMembershipOrBudget) {
  World world;
  const auto stage = Stage(0U);
  auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
  EXPECT_EQ(result.attempt_disposition, StopStageAttemptDispositionDTO::kNotDispatched);
  EXPECT_EQ(result.error->details.at("reason"),
            "resolved_stop_stage_dispatch_membership_unavailable");

  world.Dispatch(stage);
  auto other_generation = stage;
  other_generation.controller_goal_generation = ControllerGoalGeneration(2U);
  EXPECT_EQ(world.helper->TranslateStopStage(other_generation, Budget(other_generation), {})
                .error->details.at("reason"),
            "resolved_stop_stage_dispatch_membership_unavailable");
  auto other_operation = stage;
  other_operation.operation_id = OperationId("operation.other");
  EXPECT_EQ(world.helper->TranslateStopStage(other_operation, Budget(other_operation), {})
                .error->details.at("reason"),
            "resolved_stop_stage_dispatch_membership_mismatch");
  auto expired = Budget(stage);
  expired.remaining_controlled_deadline_ns = 0;
  EXPECT_EQ(world.helper->TranslateStopStage(stage, expired, {}).error->details.at("reason"),
            "resolved_stop_stage_budget_invalid");
  auto foreign = Budget(stage);
  foreign.stop_generation = StopGeneration(2U);
  EXPECT_EQ(world.helper->TranslateStopStage(stage, foreign, {}).error->details.at("reason"),
            "resolved_stop_stage_budget_invalid");
  EXPECT_TRUE(world.port.commands().empty());
  ASSERT_EQ(result.evidence_items.size(), 1U);
  EXPECT_EQ(result.evidence_items.front().state, StopEvidenceItemStateDTO::kMissing);
}

TEST(ParticipantStopStageHelperTest, CancellationBeforeDispatchIsSupersededWithoutEffect) {
  World world;
  const auto stage = Stage(0U);
  world.Dispatch(stage);
  std::stop_source source;
  source.request_stop();
  const auto result = world.helper->TranslateStopStage(stage, Budget(stage), source.get_token());
  EXPECT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSupersededNoEffect);
  EXPECT_EQ(result.error->code, "stop_stage_superseded_before_dispatch");
  EXPECT_EQ(result.evidence_items.front().state, StopEvidenceItemStateDTO::kInapplicable);
  EXPECT_TRUE(world.port.commands().empty());
}

// --- Soft Stop timeline -----------------------------------------------------------

TEST(ParticipantStopStageHelperTest, ScaleFreezeDeadlineDoesNotFabricateStopEvidence) {
  World world;
  const auto stage = Stage(0U, Nominal::kControlled, Parameters("soft_on_path", false));
  world.Dispatch(stage);
  world.Sample(0.5, 0.5, 1.0);
  world.freeze_expired = true;
  const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
  EXPECT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kFailed);
  ASSERT_TRUE(result.error);
  EXPECT_EQ(result.error->details.at("reason"), "stop_evidence_phase_expired");
  EXPECT_TRUE(Replaces(world.port).empty());
}

TEST(ParticipantStopStageHelperTest, StopReplacementWithoutExecutionIdentityIsNotSent) {
  World world;
  const auto stage = Stage(0U);
  world.helper->BindDispatchedGoal(Goal(stage), std::make_shared<ExecutionSampleAssociation>(
      ExecutionBindingEpoch(1U), world.port.ReadSample(), world.clock.now()));
  world.Executing(kOriginal);
  world.Sample(0.5, 0.5, 1.0);
  const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->details.at("reason"), "stop_execution_identity_missing");
  EXPECT_TRUE(Replaces(world.port).empty());
}

TEST(ParticipantStopStageHelperTest, SoftStopReplacesThroughThePastPointJoinAndFinishesOnSuccess) {
  World world;
  const auto stage = Stage(0U);
  world.Dispatch(stage);
  world.Sample(0.5, 0.5, 1.0);
  ScriptReplacement(world, kSecond, 5U, ExecutionGoalResult::kSucceeded);
  const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
  ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
      << result.error->message << " " << result.error->details.at("reason");
  EXPECT_EQ(result.attempt_disposition, StopStageAttemptDispositionDTO::kDispatched);
  EXPECT_EQ(result.actual_provider_contract_mode, MotionControlledStopMode::kSoftOnPath);
  EXPECT_EQ(world.freezes, 1U);
  EXPECT_TRUE(Scales(world.port).empty());  // publication stopped, nothing re-sent
  const auto replaces = Replaces(world.port);
  ASSERT_EQ(replaces.size(), 1U);
  EXPECT_EQ(replaces.front().active_key, kOriginal);
  EXPECT_EQ(replaces.front().next_key, kSecond);
  // The original's points before the elapsed time
  // 0.5 + 0.5 * (0 + 0.02) = 0.51 keep negative times; then the ramp.
  const auto& points = replaces.front().trajectory.waypoints;
  ASSERT_GE(points.size(), 5U);
  EXPECT_NEAR(points[0].time_from_start_s, -0.51, 1e-9);
  EXPECT_NEAR(points[1].time_from_start_s, -0.26, 1e-9);
  EXPECT_NEAR(points[2].time_from_start_s, -0.01, 1e-9);
  EXPECT_EQ(points[2].positions_rad, Original().waypoints[2].positions_rad);
  for (std::size_t index = 1; index < points.size(); ++index) {
    EXPECT_GT(points[index].time_from_start_s, points[index - 1U].time_from_start_s);
  }
  EXPECT_GE(points[3].time_from_start_s, 0.0);
  // The lead-in is the original path itself up to the trip point
  // 0.5 + 0.5 * 0.05 = 0.525, carried with the original's own state there.
  EXPECT_NEAR(points[3].time_from_start_s, 0.525 - 0.51, 1e-9);
  EXPECT_NEAR(points[3].positions_rad[0], 0.525, 1e-12);
  EXPECT_GT(points[4].time_from_start_s, points[3].time_from_start_s + 1e-3);
  // In path time at the frozen factor: JTC applying 0.5 gives the planned
  // wall-time ramp (lead-in at 0.5 rad/s physical, path velocity 1.0).
  EXPECT_NEAR(points[3].velocities_rad_s[0], 1.0, 1e-6);
  EXPECT_EQ(points.back().velocities_rad_s[0], 0.0);
  const auto& evidence = EvidenceObject(result);
  EXPECT_EQ(Text(evidence, "actual_stop_mode"), "soft_on_path");
  const auto& sample = std::get<JsonObjectDTO>(evidence.at("post_command_sample").value);
  EXPECT_EQ(Text(sample, "finish_authority"), "controller_reported");
  EXPECT_EQ(Text(sample, "finish_basis"), "soft_on_path_replacement_succeeded");
  const auto& diagnostics = std::get<JsonObjectDTO>(evidence.at("provider_diagnostics").value);
  EXPECT_NEAR(std::get<double>(diagnostics.at("join_elapsed_path_s").value), 0.51, 1e-9);
  EXPECT_EQ(Text(diagnostics, "path_deviation_source"), "controller_state_measured_positions");
  EXPECT_TRUE(result.source_epoch.has_value());
}

TEST(ParticipantStopStageHelperTest, OriginalKnotsInsideTheLeadInAreSentVerbatim) {
  World world;
  const auto stage = Stage(0U);
  world.Dispatch(stage);
  // Elapsed 0.735 + 0.5 * 0.02 = 0.745; trip point 0.735 + 0.5 * 0.05 = 0.76;
  // the original knot at 0.75 lies inside the lead-in.
  world.Sample(0.735, 0.735, 1.0);
  ScriptReplacement(world, kSecond, 5U, ExecutionGoalResult::kSucceeded);
  const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
  ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded);
  const auto replaces = Replaces(world.port);
  ASSERT_EQ(replaces.size(), 1U);
  const auto& points = replaces.front().trajectory.waypoints;
  ASSERT_GE(points.size(), 6U);
  const auto original = Original();
  for (std::size_t index = 0; index < 4U; ++index) {
    EXPECT_NEAR(points[index].time_from_start_s,
                original.waypoints[index].time_from_start_s - 0.745, 1e-9);
    EXPECT_EQ(points[index].positions_rad, original.waypoints[index].positions_rad);
    EXPECT_EQ(points[index].velocities_rad_s, original.waypoints[index].velocities_rad_s);
  }
  EXPECT_GT(points[3].time_from_start_s, 0.0);  // knot 0.75, ahead of the start
  EXPECT_NEAR(points[4].time_from_start_s, 0.76 - 0.745, 1e-9);
  EXPECT_NEAR(points[4].positions_rad[0], 0.76, 1e-12);
  EXPECT_NEAR(points[4].velocities_rad_s[0], 1.0, 1e-12);
  for (std::size_t index = 1; index < points.size(); ++index) {
    EXPECT_GT(points[index].time_from_start_s, points[index - 1U].time_from_start_s);
  }
}

TEST(ParticipantStopStageHelperTest, WaitsForTheFrozenFactorToBeAppliedBeforePlanning) {
  World world;
  const auto stage = Stage(0U);
  world.Dispatch(stage);
  world.Sample(0.5, 0.5, 1.0, 0.6);  // the last value sent (0.5) is not applied yet
  bool applied = false;
  world.on_tick = [&] {
    if (!applied) {
      world.Sample(0.51, 0.51, 1.0, 0.5);
      applied = true;
      return;
    }
    if (!Replaces(world.port).empty()) {
      world.Executing(kSecond);
      world.Sample(0.02, 0.6, 0.0);
      world.Finish(kSecond, ExecutionGoalResult::kSucceeded);
    }
  };
  const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
  ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded);
  // Planned from the confirming sample's cursor.
  EXPECT_NEAR(Replaces(world.port).front().trajectory.waypoints.front().time_from_start_s,
              -(0.51 + 0.5 * 0.02), 1e-9);
}

TEST(ParticipantStopStageHelperTest, UnusableStopInputsRefuseTheRampSoTheKernelHardStops) {
  struct Case {
    const char* name;
    std::function<void(World&)> setup;
    const char* reason;
  };
  const std::vector<Case> cases{
      {"no sample", [](World&) {}, "controlled_stop_no_observed_state"},
      {"factor not applied", [](World& w) { w.Sample(0.5, 0.5, 1.0, 0.7); },
       "controlled_stop_no_observed_state"},
      {"no applied factor", [](World& w) { w.Sample(0.5, 0.5, 1.0, std::nullopt); },
       "controlled_stop_no_observed_state"},
      {"non-finite sample",
       [](World& w) { w.Sample(0.5, std::nan(""), 1.0); }, "controlled_stop_no_observed_state"},
      {"sample before the goal's feedback",
       [](World& w) {
         w.Sample(0.5, 0.5, 1.0);
         w.clock.advance(5);
         w.Executing(kOriginal);
       },
       "controlled_stop_no_observed_state"},
      {"frozen at zero while the applied factor is not yet zero (ramp-down)",
       [](World& w) {
         w.frozen = 0.0;
         w.Sample(0.5, 0.5, 1.0, 0.2);
       },
       "controlled_stop_frozen_factor_unusable"},
      {"goal ended", [](World& w) { w.Finish(kOriginal, ExecutionGoalResult::kAbortedPathTolerance); },
       "prepared_goal_not_active"},
  };
  for (const auto& test : cases) {
    SCOPED_TRACE(test.name);
    World world;
    const auto stage = Stage(0U);
    world.Dispatch(stage);
    test.setup(world);
    const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
    EXPECT_EQ(result.attempt_disposition, StopStageAttemptDispositionDTO::kNotDispatched);
    EXPECT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kFailed);
    EXPECT_EQ(result.error->details.at("reason"), test.reason);
    EXPECT_FALSE(result.error->details.contains("stop_disposition"));
    EXPECT_TRUE(Replaces(world.port).empty());
  }
}

TEST(ParticipantStopStageHelperTest, StaleTripSampleCannotStartTheReplacementAfterTheLeadIn) {
  World world;
  const auto stage = Stage(0U);
  world.Dispatch(stage);
  world.Sample(0.5, 0.5, 1.0);
  world.clock.advance(40'000'000);  // 40 ms + 20 ms allowance > 50 ms lead-in
  const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
  EXPECT_EQ(result.attempt_disposition, StopStageAttemptDispositionDTO::kNotDispatched);
  EXPECT_EQ(result.error->details.at("reason"), "soft_on_path_stop_delta_budget_exceeded");
  EXPECT_EQ(result.error->details.at("recommended_disposition"), "hard_stop");
  EXPECT_TRUE(Replaces(world.port).empty());
}

TEST(ParticipantStopStageHelperTest, ReplacementStartAllowanceIsARequiredLaneInput) {
  World world(false);
  world.config.replacement_start_allowance_s.reset();
  EXPECT_THROW(world.Build(), std::invalid_argument);
  world.config.replacement_start_allowance_s = -0.001;
  EXPECT_THROW(world.Build(), std::invalid_argument);
  world.config.replacement_start_allowance_s = 0.0;
  EXPECT_NO_THROW(world.Build());
}

/// Every read advances 0.1 ms, so the read that feeds the join is visible.
class TickingClock final : public IClock {
 public:
  static constexpr Duration kReadTick = 100'000;
  explicit TickingClock(FakeClock& base) : base_(base) {}
  TimePoint now() const override {
    const auto value = base_.now();
    base_.advance(kReadTick);
    return value;
  }

 private:
  FakeClock& base_;
};

/// Forwards to the fake port and records the base clock at each Replace.
class SendTimeRecordingPort final : public IExecutionPort {
 public:
  SendTimeRecordingPort(FakeExecutionPort& port, const FakeClock& clock)
      : port_(port), clock_(clock) {}
  ExecutionPortCommandResultDTO Submit(const ExecutionGoalKey& key,
                                       const TrajectoryDTO& trajectory, std::shared_ptr<ExecutionDispatchToken> token = {}) override {
    return port_.Submit(key, trajectory, std::move(token));
  }
  ExecutionPortCommandResultDTO Replace(
      const ExecutionGoalKey& active, const ExecutionGoalKey& next,
      const TrajectoryDTO& trajectory, std::shared_ptr<ExecutionDispatchToken> token = {}) override {
    replace_clock.push_back(clock_.now());
    return port_.Replace(active, next, trajectory, std::move(token));
  }
  ExecutionPortCommandResultDTO Cancel(const ExecutionGoalKey& key, std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max()) override {
    return port_.Cancel(key, deadline);
  }
  std::uint64_t ReserveScaleSequence() noexcept override { return port_.ReserveScaleSequence(); }
  bool IsLatestScaleSequence(std::uint64_t sequence) const noexcept override { return port_.IsLatestScaleSequence(sequence); }
  ScaleSnapshot SnapshotScaleAfterFence(std::chrono::steady_clock::time_point deadline) override { return port_.SnapshotScaleAfterFence(deadline); }
  ExecutionPortCommandResultDTO SetScale(double value, std::uint64_t sequence) override { return port_.SetScale(value, sequence); }
  std::optional<ExecutionGoalObservationDTO> ReadGoal(const ExecutionGoalKey& key) const override {
    return port_.ReadGoal(key);
  }
  std::optional<ExecutionControllerSampleDTO> ReadSample() const override {
    return port_.ReadSample();
  }
  ExecutionReadinessDTO ReadReadiness() const override { return port_.ReadReadiness(); }
  ExecutionTimingDTO ReadTiming() override { return port_.ReadTiming(); }
  ExecutionLaneFactsDTO ReadLaneFacts() const override { return port_.ReadLaneFacts(); }

  std::vector<TimePoint> replace_clock;

 private:
  FakeExecutionPort& port_;
  const FakeClock& clock_;
};

TEST(ParticipantStopStageHelperTest, JoinElapsedIsMeasuredAtTheReplacementSend) {
  // Host integration: the local time since the trip sample enters the
  // join at the dispatch itself, after planning, so the replacement's time 0
  // is not older than its real start.
  World world(false);
  TickingClock ticking(world.clock);
  SendTimeRecordingPort recording(world.port, world.clock);
  world.helper = std::make_unique<ParticipantStopStageHelper>(
      recording, ticking,
      [&world](TimePoint deadline, const std::stop_token&) {
        world.clock.advance(std::max<Duration>(1, std::min(kTick, deadline - world.clock.now())));
        if (world.on_tick) world.on_tick();
      },
      [&world](auto) -> std::optional<double> { return world.frozen; }, world.config);
  const auto stage = Stage(0U);
  world.Dispatch(stage);
  world.Sample(0.5, 0.5, 1.0);
  const auto receipt = world.clock.now();
  ScriptReplacement(world, kSecond, 5U, ExecutionGoalResult::kSucceeded);
  const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
  ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded);
  ASSERT_EQ(recording.replace_clock.size(), 1U);
  // The helper's last clock read before the send is the join's: it returned
  // the send time minus the one tick that read itself advanced.
  const auto dispatch_read = recording.replace_clock.front() - TickingClock::kReadTick;
  ASSERT_GT(dispatch_read, receipt);  // planning took time on this clock
  const auto& diagnostics =
      std::get<JsonObjectDTO>(EvidenceObject(result).at("provider_diagnostics").value);
  EXPECT_NEAR(std::get<double>(diagnostics.at("join_elapsed_path_s").value),
              0.5 + 0.5 * (static_cast<double>(dispatch_read - receipt) / 1e9 + 0.02), 1e-12);
  const auto points = Replaces(world.port).front().trajectory.waypoints;
  EXPECT_NEAR(points.front().time_from_start_s,
              -(0.5 + 0.5 * (static_cast<double>(dispatch_read - receipt) / 1e9 + 0.02)), 1e-9);
}

TEST(ParticipantStopStageHelperTest, ScheduledSoftAndSpringStopsStartAtTheFirstCommand) {
  for (const char* mode : {"soft_on_path", "spring_back_hold"}) {
    SCOPED_TRACE(mode);
    World world;
    auto lane = world.port.ReadLaneFacts();
    lane.supports_scheduled_replacement = true;
    world.port.SetLaneFacts(lane);
    const auto stage = Stage(0U, Nominal::kControlled, Parameters(mode, false));
    world.Dispatch(stage);
    world.Sample(0.5, 0.5, 1.0);
    const auto source = world.port.ReadSample();
    ASSERT_TRUE(source);
    ScriptReplacement(world, kSecond, 5U, ExecutionGoalResult::kSucceeded);
    const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
    ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded);
    const auto replacements = Replaces(world.port);
    ASSERT_EQ(replacements.size(), 1U);
    const auto& replacement = replacements.front();
    ASSERT_TRUE(replacement.start);
    EXPECT_EQ(replacement.start->binding_epoch, source->binding_epoch);
    const auto advance_ns = replacement.start->controller_start_stamp_ns - source->source_stamp_ns;
    EXPECT_EQ(advance_ns, 20'000'000);
    EXPECT_EQ(advance_ns % 4'000'000, 0);
    EXPECT_EQ(replacement.active_key, kOriginal);
    EXPECT_EQ(replacement.next_key, kSecond);
    EXPECT_NEAR(replacement.trajectory.waypoints.front().time_from_start_s, 0.004, 1e-12);
    EXPECT_NEAR(replacement.trajectory.waypoints.front().positions_rad[0],
                source->cursor_s + world.frozen * static_cast<double>(advance_ns) / 1e9 + 0.004,
                1e-12);
    ASSERT_TRUE(replacement.start->experiment);
    EXPECT_EQ(replacement.start->experiment->release_hold, ExecutionReleaseHold::kHeld);
    EXPECT_EQ(world.freezes, 1U);
    EXPECT_TRUE(Cancels(world.port).empty());
  }
}

// Scheduled Stops share timing and first-command placement regardless of old flags.
TEST(ParticipantStopStageHelperTest, ScheduledStopsUseOneReleaseAndFirstCommandContract) {
  struct Case {
    bool timer, midtick, v0;
  };
  for (const char* mode : {"soft_on_path", "spring_back_hold"}) {
    for (const auto flags : {Case{true, false, false}, Case{false, true, false},
                             Case{false, false, true}, Case{true, true, true}}) {
      SCOPED_TRACE(::testing::Message() << mode << " timer=" << flags.timer
                                        << " midtick=" << flags.midtick << " v0=" << flags.v0);
      World world;
      auto lane = world.port.ReadLaneFacts();
      lane.supports_scheduled_replacement = true;
      lane.experiment.release_timer = flags.timer;
      lane.experiment.midtick_stamp = flags.midtick;
      lane.experiment.v0_payload = flags.v0;
      world.port.SetLaneFacts(lane);
      const auto stage = Stage(0U, Nominal::kControlled, Parameters(mode, false));
      world.Dispatch(stage);
      world.Sample(0.5, 0.5, 1.0);
      const auto source = world.port.ReadSample();
      ASSERT_TRUE(source);
      ScriptReplacement(world, kSecond, 5U, ExecutionGoalResult::kSucceeded);
      TimePoint unsent_until = 0;
      auto scripted = world.on_tick;
      world.on_tick = [&, scripted] {
        if (Replaces(world.port).empty()) unsent_until = world.clock.now();
        scripted();
      };
      const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
      ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded);
      const auto replacements = Replaces(world.port);
      ASSERT_EQ(replacements.size(), 1U);
      const auto& replacement = replacements.front();
      ASSERT_TRUE(replacement.start);
      ASSERT_TRUE(replacement.start->experiment);
      const auto& experiment = *replacement.start->experiment;
      constexpr TimePoint kPeriod = 4'000'000;  // 250 Hz
      // The existing whole-period target: 20 ms allowance = 5 periods.
      EXPECT_EQ(experiment.target_tick_ns, source->source_stamp_ns + 20'000'000);
      const auto advance_ns = replacement.start->controller_start_stamp_ns - source->source_stamp_ns;
      EXPECT_EQ(advance_ns, 20'000'000 + (flags.midtick ? kPeriod / 2 : 0));
      EXPECT_EQ(experiment.midtick_stamp, flags.midtick);
      EXPECT_EQ(experiment.v0_payload, flags.v0);
      EXPECT_FALSE(experiment.spring_back_fast);
      ASSERT_TRUE(experiment.release_local_ns);
      const auto release = source->local_receipt_stamp_ns + 20'000'000 - kPeriod / 2 - 1'500'000;
      EXPECT_EQ(*experiment.release_local_ns, release);
      EXPECT_EQ(experiment.release_hold, ExecutionReleaseHold::kHeld);
      EXPECT_GE(unsent_until, release);
      const double first_time = 0.004 - (flags.midtick ? world.frozen * 0.002 : 0.0);
      const auto& first = replacement.trajectory.waypoints.front();
      EXPECT_NEAR(first.time_from_start_s, first_time, 1e-12);
      EXPECT_NEAR(first.positions_rad[0], source->cursor_s + world.frozen * 0.020 + 0.004,
                  1e-12);
      if (std::string(mode) == "spring_back_hold") {
        EXPECT_NEAR(replacement.trajectory.waypoints.back().positions_rad[0], 0.5, 1e-9);
      }
    }
  }
}

TEST(ParticipantStopStageHelperTest, ReleaseHoldIsCancellableAndNeverCrossesTheStopDeadline) {
  for (const bool cancel : {true, false}) {
    SCOPED_TRACE(cancel ? "cancelled during hold" : "Stop deadline before release");
    World world;
    auto lane = world.port.ReadLaneFacts();
    lane.supports_scheduled_replacement = true;
    lane.experiment.release_timer = true;
    world.port.SetLaneFacts(lane);
    const auto stage = Stage(0U, Nominal::kControlled, Parameters("soft_on_path", false));
    world.Dispatch(stage);
    world.Sample(0.5, 0.5, 1.0);
    ScriptReplacement(world, kSecond, 5U, ExecutionGoalResult::kSucceeded);
    std::stop_source source;
    auto scripted = world.on_tick;
    world.on_tick = [&, scripted] {
      if (cancel && Replaces(world.port).empty()) source.request_stop();
      scripted();
    };
    auto budget = Budget(stage);
    if (!cancel) {
      // 10 ms of Stop time left; the release lies 16.5 ms after the dispatch.
      const auto& command = stage.participant_stop_plan.stage_commands.at(stage.stage_index);
      const auto deadline =
          controlled_stop_deadline_from_contract_parameters(*command.validated_contract_parameters);
      ASSERT_TRUE(deadline);
      budget = Budget(stage, *deadline - 10'000'000);
    }
    const auto result = world.helper->TranslateStopStage(stage, budget, source.get_token());
    const auto replacements = Replaces(world.port);
    if (cancel) {
      // Cancellation preempts the hold before anything is sent.
      EXPECT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSupersededNoEffect);
      EXPECT_TRUE(replacements.empty());
    } else {
      EXPECT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kFailed);
      EXPECT_TRUE(replacements.empty());
      ASSERT_TRUE(result.error);
      EXPECT_EQ(result.error->details.at("reason"), "soft_on_path_stop_delta_budget_exceeded");
    }
  }
}

TEST(ParticipantStopStageHelperTest, ReleaseHoldOversleepPastHalfAPeriodSendsNothing) {
  // After waking, the ACTUAL time is checked against the existing
  // release plus half-period, even inside the old 50ms lead-in: this is the
  // existing lead-in budget refusal, before any send.
  World world(false);
  auto lane = world.port.ReadLaneFacts();
  lane.supports_scheduled_replacement = true;
  lane.experiment.release_timer = true;
  world.port.SetLaneFacts(lane);
  world.helper = std::make_unique<ParticipantStopStageHelper>(
      world.port, world.clock,
      [&world](TimePoint, const std::stop_token&) {
        world.clock.advance(19'000'000);  // release16.5ms + halfperiod2ms is already past
        ++world.ticks;
      },
      [&world](auto) -> std::optional<double> { return world.frozen; }, world.config);
  const auto stage = Stage(0U, Nominal::kControlled, Parameters("soft_on_path", false));
  world.Dispatch(stage);
  world.Sample(0.5, 0.5, 1.0);
  const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
  EXPECT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kFailed);
  ASSERT_TRUE(result.error);
  EXPECT_EQ(result.error->details.at("reason"), "soft_on_path_stop_delta_budget_exceeded");
  EXPECT_TRUE(Replaces(world.port).empty());
  EXPECT_EQ(world.ticks, 1U);
}

TEST(ParticipantStopStageHelperTest, ScheduledFastSpringBackSendsTheV0ShapeOnAFutureStamp) {
  for (const bool other_flags : {false, true}) {
    SCOPED_TRACE(other_flags ? "with timer and mid-tick" : "alone");
    World world;
    auto lane = world.port.ReadLaneFacts();
    lane.supports_scheduled_replacement = true;
    lane.experiment.scheduled_spring_back_fast = true;
    lane.experiment.release_timer = other_flags;
    lane.experiment.midtick_stamp = other_flags;
    world.port.SetLaneFacts(lane);
    const auto stage = Stage(0U, Nominal::kControlled, Parameters("spring_back_fast", false));
    world.Dispatch(stage);
    world.Sample(0.5, 0.5, 1.0);
    const auto source = world.port.ReadSample();
    ASSERT_TRUE(source);
    ScriptReplacement(world, kSecond, 5U, ExecutionGoalResult::kSucceeded);
    const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
    ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
        << result.error->message << " " << result.error->details.at("reason");
    EXPECT_EQ(result.actual_provider_contract_mode, MotionControlledStopMode::kSpringBackFast);
    const auto replaces = Replaces(world.port);
    ASSERT_EQ(replaces.size(), 1U);
    const auto& replacement = replaces.front();
    ASSERT_TRUE(replacement.start);
    ASSERT_TRUE(replacement.start->experiment);
    EXPECT_TRUE(replacement.start->experiment->spring_back_fast);
    EXPECT_TRUE(replacement.start->experiment->release_local_ns.has_value());
    const auto advance_ns = replacement.start->controller_start_stamp_ns - source->source_stamp_ns;
    EXPECT_EQ(advance_ns, 20'000'000 + (other_flags ? 2'000'000 : 0));
    const auto& diagnostics =
        std::get<JsonObjectDTO>(EvidenceObject(result).at("provider_diagnostics").value);
    const double start = std::get<double>(diagnostics.at("scheduled_start_path_s").value);
    EXPECT_NEAR(start, 0.5 + 0.5 * static_cast<double>(advance_ns) / 1e9, 1e-12);
    EXPECT_EQ(Text(EvidenceObject(result), "actual_stop_mode"), "spring_back_fast");
    EXPECT_EQ(std::get<double>(diagnostics.at("passed_points").value), 0.0);
    const auto& points = replacement.trajectory.waypoints;
    ASSERT_EQ(points.size(), 2U);
    const double first_time = 0.004 - (other_flags ? 0.001 : 0.0);
    EXPECT_NEAR(points.front().time_from_start_s, first_time, 1e-12);
    EXPECT_NEAR(points.front().positions_rad[0], 0.514, 1e-12);
    EXPECT_NEAR(points.front().velocities_rad_s[0], 1.0, 1e-12);
    EXPECT_NEAR(points.back().time_from_start_s, first_time + 0.5, 1e-12);
    EXPECT_NEAR(points.back().positions_rad[0], 0.5, 1e-12);
    EXPECT_EQ(points.back().velocities_rad_s, std::vector<double>{0.0});
    EXPECT_EQ(points.back().accelerations_rad_s2, std::vector<double>{0.0});
  }
}

TEST(ParticipantStopStageHelperTest, OnlyUnscheduledFastSpringBackKeepsTheLegacyPrefix) {
  // Capability, rather than a per-mode flag, chooses the common scheduled path.
  for (const bool scheduled_lane : {false, true}) {
    SCOPED_TRACE(scheduled_lane);
    World world;
    auto lane = world.port.ReadLaneFacts();
    lane.supports_scheduled_replacement = scheduled_lane;
    lane.experiment.scheduled_spring_back_fast = !scheduled_lane;
    lane.experiment.release_timer = true;
    lane.experiment.midtick_stamp = true;
    lane.experiment.v0_payload = true;
    world.port.SetLaneFacts(lane);
    const auto stage = Stage(0U, Nominal::kControlled, Parameters("spring_back_fast", false));
    world.Dispatch(stage);
    world.Sample(0.5, 0.5, 1.0);
    ScriptReplacement(world, kSecond, 5U, ExecutionGoalResult::kSucceeded);
    const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
    ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded);
    const auto replaces = Replaces(world.port);
    ASSERT_EQ(replaces.size(), 1U);
    const auto& points = replaces.front().trajectory.waypoints;
    ASSERT_GE(points.size(), 2U);
    if (scheduled_lane) {
      ASSERT_TRUE(replaces.front().start);
      EXPECT_NEAR(points.front().time_from_start_s, 0.003, 1e-12);
      EXPECT_NEAR(points.back().time_from_start_s, 0.503, 1e-12);
    } else {
      EXPECT_FALSE(replaces.front().start);
      EXPECT_LT(points.front().time_from_start_s, 0.0);
      EXPECT_NEAR(points.back().time_from_start_s, 0.5, 1e-12);
      const auto& diagnostics =
          std::get<JsonObjectDTO>(EvidenceObject(result).at("provider_diagnostics").value);
      EXPECT_FALSE(diagnostics.contains("spring_back_fast_shape"));
    }
  }
}

TEST(ParticipantStopStageHelperTest, ReplacementNotAcceptedWithinTheLeadInIsCancelled) {
  World world;
  const auto stage = Stage(0U);
  world.Dispatch(stage);
  world.Sample(0.5, 0.5, 1.0);
  const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
  EXPECT_EQ(result.attempt_disposition, StopStageAttemptDispositionDTO::kDispatched);
  EXPECT_EQ(result.actual_provider_contract_mode, MotionControlledStopMode::kSoftOnPath);
  EXPECT_EQ(result.error->details.at("reason"), "soft_on_path_stop_delta_budget_exceeded");
  EXPECT_EQ(Cancels(world.port), std::vector<ExecutionGoalKey>{kSecond});
}

TEST(ParticipantStopStageHelperTest, RefusedOrRejectedReplacementFailsTheStage) {
  {
    World world;
    const auto stage = Stage(0U);
    world.Dispatch(stage);
    world.Sample(0.5, 0.5, 1.0);
    world.port.QueueCommandResult({ExecutionPortDispatch::kNotDispatched,
                                   ExecutionPortRefusal::kNotReady, std::nullopt});
    const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
    EXPECT_EQ(result.attempt_disposition, StopStageAttemptDispositionDTO::kNotDispatched);
    EXPECT_EQ(result.error->details.at("reason"), "soft_on_path_path_ramp_dispatch_failed");
  }
  World world;
  const auto stage = Stage(0U);
  world.Dispatch(stage);
  world.Sample(0.5, 0.5, 1.0);
  world.on_tick = [&] {
    if (!Replaces(world.port).empty()) world.Finish(kSecond, ExecutionGoalResult::kRejected);
  };
  const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
  EXPECT_EQ(result.attempt_disposition, StopStageAttemptDispositionDTO::kDispatched);
  EXPECT_EQ(result.error->details.at("reason"), "soft_on_path_path_ramp_dispatch_failed");
  // The later hard stage still cancels the original, which keeps running.
  world.on_tick = {};
  world.Sample(0.6, 0.6, 1.0);
  world.port.QueueCommandResult({ExecutionPortDispatch::kDispatched, std::nullopt, std::nullopt});
  world.port.QueueCommandResult({ExecutionPortDispatch::kNotDispatched,
                                 ExecutionPortRefusal::kStaleKey, std::nullopt});
  const auto hard = Stage(1U);
  (void)world.helper->TranslateStopStage(hard, Budget(hard, 100'000'000), {});
  EXPECT_EQ(Cancels(world.port), std::vector<ExecutionGoalKey>{kOriginal});
}

// A sent Stop trajectory finishes like a normal motion, on the
// controller's report, whenever it comes inside a normal motion's no-report
// bound (here 10 s from the send: the trajectory lasts under 5.3 s), well past
// the profile's 1.5 s deadline to start braking. Within the arrival tolerance
// it succeeds; outside it takes the goal-tolerance path; with no report it
// expires at the normal-motion bound, never before.
TEST(ParticipantStopStageHelperTest, SentStopTrajectoryFinishesLikeANormalMotion) {
  for (const auto ended : {std::optional(ExecutionGoalResult::kSucceeded),
                           std::optional(ExecutionGoalResult::kAbortedGoalTolerance),
                           std::optional<ExecutionGoalResult>()}) {
    SCOPED_TRACE(ended ? std::string(to_string(*ended)) : std::string("no report"));
    World world;
    world.Lane(ExecutionGoalTolerancePolicy::kKept);
    const auto stage = Stage(0U);
    world.Dispatch(stage);
    world.Sample(0.5, 0.5, 1.0);
    const auto t0 = world.clock.now();
    std::optional<TimePoint> sent_seen;
    std::size_t count = 0U;
    bool finished = false;
    world.on_tick = [&] {
      if (Replaces(world.port).empty() || finished) return;
      if (++count == 1U) {
        sent_seen = world.clock.now();
        world.Executing(kSecond);
        world.port.FinishGoal(kOriginal, ExecutionGoalResult::kPreemptedBy, kSecond,
                              world.clock.now());
      }
      world.Sample(0.01 * static_cast<double>(count), 0.5, 0.0);
      // The controller's verdict, late (its settle window), past 1.5 s.
      if (ended && world.clock.now() >= t0 + 3'000'000'000) {
        world.Finish(kSecond, *ended);
        finished = true;
      }
    };
    const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
    EXPECT_EQ(result.attempt_disposition, StopStageAttemptDispositionDTO::kDispatched);
    EXPECT_TRUE(Cancels(world.port).empty());
    ASSERT_TRUE(sent_seen.has_value());
    if (ended == ExecutionGoalResult::kSucceeded) {
      ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
          << result.error->details.at("reason");
      continue;
    }
    EXPECT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kFailed);
    ASSERT_TRUE(result.error.has_value());
    if (ended) {
      EXPECT_EQ(result.error->details.at("reason"), "stop_goal_tolerance_violated");
      continue;
    }
    EXPECT_EQ(result.error->details.at("reason"), "backend_stop_not_confirmed");
    // max(10 s, 1.5 x duration + 2 s) from the send (one tick before seen).
    EXPECT_GE(world.clock.now(), *sent_seen - kTick + 10'000'000'000);
    EXPECT_LE(world.clock.now(), *sent_seen + 10'000'000'000);
  }
}

TEST(ParticipantStopStageHelperTest, AConsumedPlanningLeadCanSelectAnEnabledImmediateFastStop) {
  World world(false);
  world.config.controller_state_max_age_ns = 1'000'000'000;
  world.Build();
  const auto stage =
      Stage(0U, Nominal::kControlled, FallbackParameters("soft_on_path", {"spring_back_fast"}));
  world.Dispatch(stage);
  world.Sample(0.5, 0.5, 1.0);
  std::size_t reads = 0U;
  world.port.SetOnReadSample([&](auto&) {
    if (++reads == 4U) world.clock.advance(100'000'000);
  });
  ScriptReplacement(world, kSecond, 5U, ExecutionGoalResult::kSucceeded);
  const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
  ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded) << result.error->message;
  EXPECT_EQ(result.actual_provider_contract_mode, MotionControlledStopMode::kSpringBackFast);
  EXPECT_EQ(result.recoverable_substitution_reason, "soft_on_path_stop_delta_budget_exceeded");
  ASSERT_EQ(Replaces(world.port).size(), 1U);
  EXPECT_EQ(Replaces(world.port).front().next_key, kSecond);
  EXPECT_EQ(world.freezes, 1U);
}

TEST(ParticipantStopStageHelperTest, OrderedFallbacksKeepTheOriginalAbsoluteDeadline) {
  World world(false);
  world.config.controller_state_max_age_ns = 3'000'000'000;
  world.Build();
  const auto stage =
      Stage(0U, Nominal::kControlled,
            FallbackParameters("soft_on_path", {"spring_back_hold", "spring_back_fast"}));
  world.Dispatch(stage);
  world.Sample(0.5, 0.5, 1.0);
  const auto started = world.clock.now();
  std::size_t reads = 0U;
  world.port.SetOnReadSample([&](auto&) {
    // The pre-send baseline read after frozen trip capture. Model expensive
    // preparation against the same clock; no extra wait/deadline is created.
    if (++reads == 4U) world.clock.advance(1'600'000'000);
  });
  const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
  ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kFailed);
  ASSERT_TRUE(result.error);
  EXPECT_EQ(result.error->details.at("reason"), "controlled_stop_deadline_exceeded");
  EXPECT_TRUE(result.error->details.at("fallback_attempt_1")
                  .starts_with("soft_on_path:soft_on_path_stop_delta_budget_exceeded"));
  EXPECT_EQ(world.clock.now(), started + 1'600'000'000);
  EXPECT_TRUE(Replaces(world.port).empty());
  EXPECT_EQ(world.freezes, 1U);
}

// --- Natural completion, spring-back and planner-chosen hard stop ------------------

TEST(ParticipantStopStageHelperTest, NaturalCompletionLetsTheOriginalGoalFinish) {
  World world;
  const auto stage = Stage(0U);
  world.Dispatch(stage);
  world.Sample(1.9, 1.9, 1.0);
  world.on_tick = [&] {
    if (world.ticks == 5U) world.Finish(kOriginal, ExecutionGoalResult::kSucceeded);
    world.Sample(1.95, 1.95, world.ticks >= 5U ? 0.0 : 1.0);
  };
  const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
  ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded);
  EXPECT_TRUE(Replaces(world.port).empty());
  EXPECT_EQ(result.actual_provider_contract_mode, MotionControlledStopMode::kSoftOnPath);
  const auto& evidence = EvidenceObject(result);
  EXPECT_TRUE(std::get<bool>(evidence.at("completed_by_natural_completion").value));
}

TEST(ParticipantStopStageHelperTest, GoalCompletedBeforeTheStageIsNaturalCompletionOnlyIfAllowed) {
  for (const bool allowed : {true, false}) {
    SCOPED_TRACE(allowed);
    World world;
    const auto stage = Stage(0U, Nominal::kControlled, Parameters("soft_on_path", allowed));
    world.Dispatch(stage);
    world.Finish(kOriginal, ExecutionGoalResult::kSucceeded);
    world.clock.advance(1);
    world.Sample(2.0, 2.0, 0.0);
    const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
    EXPECT_TRUE(world.port.commands().empty());
    if (allowed) {
      EXPECT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded);
    } else {
      EXPECT_EQ(result.error->details.at("reason"), "prepared_goal_not_active");
      EXPECT_EQ(result.attempt_disposition, StopStageAttemptDispositionDTO::kNotDispatched);
    }
  }
}

TEST(ParticipantStopStageHelperTest, SpringBackSubstitutionUsesTheSameReplaceTimeline) {
  World world;
  const auto stage = Stage(0U, Nominal::kControlled,
                           Parameters("soft_on_path", false, std::string("spring_back_hold")));
  world.Dispatch(stage);
  world.Sample(1.8, 1.8, 1.0);
  ScriptReplacement(world, kSecond, 5U, ExecutionGoalResult::kSucceeded);
  const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
  ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
      << result.error->details.at("reason");
  EXPECT_EQ(result.actual_provider_contract_mode, MotionControlledStopMode::kSpringBackHold);
  EXPECT_EQ(result.recoverable_substitution_reason, "soft_on_path_remaining_time_short");
  const auto replaces = Replaces(world.port);
  ASSERT_EQ(replaces.size(), 1U);
  EXPECT_LT(replaces.front().trajectory.waypoints.front().time_from_start_s, 0.0);
  // The return ends at the exact trip cursor's pose.
  EXPECT_NEAR(replaces.front().trajectory.waypoints.back().positions_rad[0], 1.8, 1e-6);
  const auto& diagnostics =
      std::get<JsonObjectDTO>(EvidenceObject(result).at("provider_diagnostics").value);
  EXPECT_EQ(Text(diagnostics, "recoverable_substitution_pair"), "soft_on_path->spring_back_hold");
}

TEST(ParticipantStopStageHelperTest, OrderedFallbacksAdvanceOnlyThroughEnabledRefusedModes) {
  struct Case {
    std::string primary;
    std::vector<std::string> fallbacks;
    double cursor;
    double deceleration;
    MotionControlledStopMode expected;
  };
  for (const auto& row : std::vector<Case>{{"soft_on_path",
                                            {"spring_back_hold"},
                                            1.8,
                                            4.0,
                                            MotionControlledStopMode::kSpringBackHold},
                                           {"spring_back_hold",
                                            {"spring_back_fast"},
                                            0.5,
                                            0.01,
                                            MotionControlledStopMode::kSpringBackFast},
                                           {"soft_on_path",
                                            {"spring_back_fast"},
                                            0.5,
                                            0.01,
                                            MotionControlledStopMode::kSpringBackFast}}) {
    SCOPED_TRACE(row.primary);
    World world;
    const auto stage = Stage(0U, Nominal::kControlled,
                             FallbackParameters(row.primary, row.fallbacks, row.deceleration));
    world.Dispatch(stage);
    world.Sample(row.cursor, row.cursor, 1.0);
    ScriptReplacement(world, kSecond, 5U, ExecutionGoalResult::kSucceeded);
    const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
    ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded) << result.error->message;
    EXPECT_EQ(result.actual_provider_contract_mode, row.expected);
    EXPECT_EQ(Replaces(world.port).size(), 1U);
    EXPECT_EQ(world.freezes, 1U);
    const auto& diagnostics =
        std::get<JsonObjectDTO>(EvidenceObject(result).at("provider_diagnostics").value);
    EXPECT_TRUE(Text(diagnostics, "fallback_attempt_1").starts_with(row.primary + ":"));
  }
}

TEST(ParticipantStopStageHelperTest, FallbacksNeverMaskDriftOrAnEffectfulExecutionFailure) {
  {
    World world(false);
    world.config.trip_authority = StateAuthorityClass::kMeasured;
    world.config.max_initial_path_deviation_rad = 0.01;
    world.Build();
    const auto stage =
        Stage(0U, Nominal::kControlled,
              FallbackParameters("soft_on_path", {"spring_back_hold", "spring_back_fast"}));
    world.Dispatch(stage);
    world.Sample(0.5, 0.5, 1.0, 0.5, -0.5);
    const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
    ASSERT_TRUE(result.error);
    EXPECT_EQ(result.error->details.at("reason"), "initial_path_deviation_exceeds_limit");
    EXPECT_TRUE(Replaces(world.port).empty());
  }
  for (const unsigned failure : {0U, 1U, 2U}) {
    SCOPED_TRACE(failure);
    World world;
    const auto stage =
        Stage(0U, Nominal::kControlled,
              FallbackParameters("soft_on_path", {"spring_back_hold", "spring_back_fast"}));
    world.Dispatch(stage);
    world.Sample(0.5, 0.5, 1.0);
    if (failure == 1U) {
      world.port.QueueCommandResult({ExecutionPortDispatch::kEffectUncertain, std::nullopt,
                                     ErrorDTO{"send_unknown", "effect unknown", {}}});
    } else if (failure == 2U) {
      world.port.QueueCommandResult(
          {ExecutionPortDispatch::kNotDispatched, ExecutionPortRefusal::kStaleKey, std::nullopt});
    } else {
      ScriptReplacement(world, kSecond, 5U, ExecutionGoalResult::kAbortedPathTolerance);
    }
    const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
    EXPECT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kFailed);
    EXPECT_EQ(result.attempt_disposition, failure == 2U
                                              ? StopStageAttemptDispositionDTO::kNotDispatched
                                              : StopStageAttemptDispositionDTO::kDispatched);
    EXPECT_EQ(Replaces(world.port).size(), 1U);
  }
}

class SpringPointObserver final : public IStopStageSpringReturnObserver {
 public:
  explicit SpringPointObserver(World& world, bool accept = true) : world_(world), accept_(accept) {}
  bool SpringReturnPointReady(const SpringBackReturnPointDTO& value) noexcept override {
    ++calls;
    try {
      after_replace = !Replaces(world_.port).empty();
      if (!accept_) return false;
      point = value;
      return true;
    } catch (...) {
      return false;
    }
  }
  std::optional<SpringBackReturnPointDTO> point;
  unsigned calls{};
  bool after_replace{};

 private:
  World& world_;
  bool accept_;
};

TEST(ParticipantStopStageHelperTest,
     TakeoverUsesOnlySealedSpringPermissionAndPublishesAfterRecognition) {
  for (const bool substituted : {false, true}) {
    World world;
    const auto stage =
        Stage(0U, Nominal::kControlled,
              substituted ? Parameters("soft_on_path", false, std::string("spring_back_hold"))
                          : Parameters("spring_back_hold", false));
    world.Dispatch(stage);
    world.Sample(0.5, 0.5, 1.0);
    ScriptReplacement(world, kSecond, 5U, ExecutionGoalResult::kSucceeded);
    SpringPointObserver observer(world);
    const auto result =
        world.helper->TranslateStopStageForTakeover(stage, Budget(stage), {}, observer);
    ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
        << result.error->details.at("reason");
    EXPECT_EQ(result.actual_provider_contract_mode, MotionControlledStopMode::kSpringBackHold);
    EXPECT_EQ(observer.calls, 1U);
    EXPECT_TRUE(observer.after_replace);
    ASSERT_TRUE(observer.point);
    EXPECT_EQ(observer.point->source_goal, kOriginal);
    EXPECT_EQ(observer.point->binding_epoch, ExecutionBindingEpoch(1U));
    EXPECT_NEAR(observer.point->initial.command_path_time_s, 0.5, 1e-12);
    EXPECT_EQ(observer.point->initial.spring_back_return_point.positions_rad,
              std::vector<double>{0.5});
    EXPECT_EQ(observer.point->initial.spring_back_return_point.velocities_rad_s,
              std::vector<double>{0.0});
    EXPECT_EQ(observer.point->initial.spring_back_return_point.accelerations_rad_s2,
              std::vector<double>{0.0});
    ASSERT_EQ(Replaces(world.port).size(), 1U);
    EXPECT_NEAR(Replaces(world.port).front().trajectory.waypoints.back().positions_rad.front(), 0.5,
                1e-6);
    EXPECT_EQ(result.recoverable_substitution_reason.has_value(), substituted);
  }
}

TEST(ParticipantStopStageHelperTest, TakeoverHoldRefusalPublishesOnlyTheAcceptedFastReturnPoint) {
  World world(false);
  world.config.trip_authority = StateAuthorityClass::kMeasured;
  world.Build();
  const auto stage = Stage(0U, Nominal::kControlled,
                           FallbackParameters("spring_back_hold", {"spring_back_fast"}, 0.01));
  world.Dispatch(stage);
  world.Sample(0.5, 0.5, 1.0, 0.5, 0.49);
  ScriptReplacement(world, kSecond, 5U, ExecutionGoalResult::kSucceeded);
  SpringPointObserver observer(world);
  const auto result =
      world.helper->TranslateStopStageForTakeover(stage, Budget(stage), {}, observer);
  ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded) << result.error->message;
  EXPECT_EQ(result.actual_provider_contract_mode, MotionControlledStopMode::kSpringBackFast);
  EXPECT_EQ(observer.calls, 1U);
  EXPECT_TRUE(observer.after_replace);
  ASSERT_TRUE(observer.point);
  EXPECT_EQ(observer.point->initial.spring_back_return_point.positions_rad,
            std::vector<double>{0.49});
  ASSERT_EQ(Replaces(world.port).size(), 1U);
  EXPECT_EQ(Replaces(world.port).front().trajectory.waypoints.back().positions_rad,
            observer.point->initial.spring_back_return_point.positions_rad);
}

TEST(ParticipantStopStageHelperTest, TakeoverWithoutSpringPermissionKeepsOrdinaryStopAndNoAnchor) {
  World world;
  const auto stage = Stage(0U, Nominal::kControlled, Parameters("soft_on_path", false));
  world.Dispatch(stage);
  world.Sample(0.5, 0.5, 1.0);
  ScriptReplacement(world, kSecond, 5U, ExecutionGoalResult::kSucceeded);
  SpringPointObserver observer(world);
  const auto result =
      world.helper->TranslateStopStageForTakeover(stage, Budget(stage), {}, observer);
  EXPECT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded);
  EXPECT_EQ(result.actual_provider_contract_mode, MotionControlledStopMode::kSoftOnPath);
  EXPECT_EQ(observer.calls, 0U);
  EXPECT_FALSE(observer.point);
}

TEST(ParticipantStopStageHelperTest, SpringNotificationFailureNeverChangesContainment) {
  World world;
  const auto stage = Stage(0U, Nominal::kControlled, Parameters("spring_back_hold", false));
  world.Dispatch(stage);
  world.Sample(0.5, 0.5, 1.0);
  ScriptReplacement(world, kSecond, 5U, ExecutionGoalResult::kSucceeded);
  SpringPointObserver observer(world, false);
  const auto result =
      world.helper->TranslateStopStageForTakeover(stage, Budget(stage), {}, observer);
  EXPECT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded);
  EXPECT_EQ(result.actual_provider_contract_mode, MotionControlledStopMode::kSpringBackHold);
  EXPECT_EQ(observer.calls, 1U);
  EXPECT_FALSE(observer.point);
  ASSERT_EQ(Replaces(world.port).size(), 1U);
  EXPECT_NEAR(Replaces(world.port).front().trajectory.waypoints.back().positions_rad.front(), 0.5,
              1e-6);
}

TEST(ParticipantStopStageHelperTest, RefusedSpringDoesNotPublishAnUncommittedReturnPoint) {
  World world;
  const auto stage =
      Stage(0U, Nominal::kControlled, Parameters("spring_back_hold", false, std::nullopt, 0.01));
  world.Dispatch(stage);
  world.Sample(0.5, 0.5, 1.0);
  SpringPointObserver observer(world);
  const auto result =
      world.helper->TranslateStopStageForTakeover(stage, Budget(stage), {}, observer);
  EXPECT_EQ(observer.calls, 0U);
  EXPECT_FALSE(observer.point);
  EXPECT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kFailed);
  EXPECT_TRUE(Replaces(world.port).empty());
}

TEST(ParticipantStopStageHelperTest, DirectSpringBackProfileUsesTheSameReplaceTimeline) {
  World world;
  const auto stage = Stage(0U, Nominal::kControlled, Parameters("spring_back_hold", false));
  world.Dispatch(stage);
  world.Sample(0.5, 0.5, 1.0);
  ScriptReplacement(world, kSecond, 5U, ExecutionGoalResult::kSucceeded);
  const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
  ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
      << result.error->details.at("reason");
  EXPECT_EQ(result.actual_provider_contract_mode, MotionControlledStopMode::kSpringBackHold);
  EXPECT_FALSE(result.recoverable_substitution_reason.has_value());
  const auto replaces = Replaces(world.port);
  ASSERT_EQ(replaces.size(), 1U);
  EXPECT_NEAR(replaces.front().trajectory.waypoints.back().positions_rad[0], 0.5, 1e-6);
}

// The fast spring-back sends the passed points plus one zero-velocity
// point at the trip position, with no planner and no lead-in ramp.
TEST(ParticipantStopStageHelperTest, FastSpringBackSendsPassedPointsAndOneTripPointAtOnce) {
  World world;
  const auto stage = Stage(0U, Nominal::kControlled, Parameters("spring_back_fast", false));
  world.Dispatch(stage);
  world.Sample(0.5, 0.5, 1.0);
  ScriptReplacement(world, kSecond, 5U, ExecutionGoalResult::kSucceeded);
  const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
  ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
      << result.error->message << " " << result.error->details.at("reason");
  EXPECT_EQ(result.actual_provider_contract_mode, MotionControlledStopMode::kSpringBackFast);
  const auto replaces = Replaces(world.port);
  ASSERT_EQ(replaces.size(), 1U);
  EXPECT_EQ(replaces.front().active_key, kOriginal);
  const auto& evidence = EvidenceObject(result);
  EXPECT_EQ(Text(evidence, "actual_stop_mode"), "spring_back_fast");
  const auto& diagnostics = std::get<JsonObjectDTO>(evidence.at("provider_diagnostics").value);
  const double elapsed = std::get<double>(diagnostics.at("join_elapsed_path_s").value);
  const auto& points = replaces.front().trajectory.waypoints;
  const auto original = Original();
  std::size_t passed = 0U;
  while (original.waypoints[passed].time_from_start_s < elapsed) ++passed;
  ASSERT_EQ(points.size(), passed + 1U);
  for (std::size_t index = 0U; index < passed; ++index) {
    EXPECT_NEAR(points[index].time_from_start_s,
                original.waypoints[index].time_from_start_s - elapsed, 1e-12);
    EXPECT_LT(points[index].time_from_start_s, 0.0);
    EXPECT_EQ(points[index].positions_rad, original.waypoints[index].positions_rad);
  }
  // Deceleration time 1.0 s in wall time at the frozen factor 0.5.
  EXPECT_NEAR(points.back().time_from_start_s, 0.5, 1e-12);
  EXPECT_NEAR(points.back().positions_rad[0], 0.5, 1e-12);
  EXPECT_EQ(points.back().velocities_rad_s, std::vector<double>{0.0});
  EXPECT_EQ(points.back().accelerations_rad_s2, std::vector<double>{0.0});
}

// With no point passed yet the message starts now, at the trip position, so
// the controller takes it at once rather than as a splice.
TEST(ParticipantStopStageHelperTest, FastSpringBackBeforeAnyPassedPointStartsNowAtTheTripPosition) {
  World world;
  const auto stage = Stage(0U, Nominal::kControlled, Parameters("spring_back_fast", false));
  world.Dispatch(stage);
  world.Sample(0.0, 0.0, 1.0);
  ScriptReplacement(world, kSecond, 5U, ExecutionGoalResult::kSucceeded);
  const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
  ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
      << result.error->message << " " << result.error->details.at("reason");
  const auto& evidence = EvidenceObject(result);
  const auto& diagnostics = std::get<JsonObjectDTO>(evidence.at("provider_diagnostics").value);
  EXPECT_EQ(std::get<double>(diagnostics.at("passed_points").value), 0.0);
  const auto replaces = Replaces(world.port);
  ASSERT_EQ(replaces.size(), 1U);
  const auto& points = replaces.front().trajectory.waypoints;
  ASSERT_EQ(points.size(), 2U);
  EXPECT_EQ(points.front().time_from_start_s, 0.0);
  EXPECT_EQ(points.front().positions_rad, std::vector<double>{0.0});
  EXPECT_EQ(points.front().velocities_rad_s, std::vector<double>{0.0});
  EXPECT_NEAR(points.back().time_from_start_s, 0.5, 1e-12);
  EXPECT_EQ(points.back().positions_rad, std::vector<double>{0.0});
}

TEST(ParticipantStopStageHelperTest, FastSpringBackLetsAShortRemainingMoveFinishWhenAllowed) {
  World world;
  const auto stage = Stage(0U, Nominal::kControlled, Parameters("spring_back_fast", true));
  world.Dispatch(stage);
  world.Sample(1.9, 1.9, 1.0);
  world.on_tick = [&] {
    if (world.ticks == 5U) world.Finish(kOriginal, ExecutionGoalResult::kSucceeded);
    world.Sample(1.95, 1.95, world.ticks >= 5U ? 0.0 : 1.0);
  };
  const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
  ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded);
  EXPECT_TRUE(Replaces(world.port).empty());
  const auto& evidence = EvidenceObject(result);
  EXPECT_TRUE(std::get<bool>(evidence.at("completed_by_natural_completion").value));
}

/// The replacement's cursor maps back to the original path through the
/// Stop planner's own sample mapping; after the replacement's success (and its
/// hold, cursor reset) the original cursor stays frozen at the mapped end.
TEST(ParticipantStopStageHelperTest, ReplacementCursorMapsToTheOriginalPathAndFreezesAtTheHold) {
  for (const bool spring_back : {false, true}) {
    SCOPED_TRACE(spring_back ? "spring-back" : "soft on path");
    World world;
    const auto stage = Stage(0U, Nominal::kControlled,
                             spring_back ? Parameters("spring_back_hold", false)
                                         : Parameters("soft_on_path"));
    world.Dispatch(stage);
    world.Sample(0.5, 0.5, 1.0);
    EXPECT_FALSE(world.helper->OriginalPathCursor(world.port.ReadSample()).has_value());
    ScriptReplacement(world, kSecond, 100U, ExecutionGoalResult::kSucceeded);
    std::vector<double> mapped;
    const auto script = world.on_tick;
    world.on_tick = [&] {
      script();
      if (const auto cursor = world.helper->OriginalPathCursor(world.port.ReadSample())) {
        mapped.push_back(*cursor);
      }
    };
    const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
    ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
        << result.error->details.at("reason");
    ASSERT_GE(mapped.size(), 10U);
    // Before the trip point the replacement carries the original's own time.
    EXPECT_NEAR(mapped.front(), 0.51 + 0.01, 1e-9);
    const auto end = world.helper->OriginalPathCursor(std::nullopt);
    ASSERT_TRUE(end.has_value());
    // The success hold restarts the controller's cursor; the original stays.
    world.Sample(0.0, 0.5, 0.0);
    EXPECT_EQ(world.helper->OriginalPathCursor(world.port.ReadSample()), end);
    if (spring_back) {
      // Braking forward, then back along the path to the exact trip cursor.
      const auto peak = *std::max_element(mapped.begin(), mapped.end());
      EXPECT_GT(peak, 0.525);
      EXPECT_LT(mapped.back(), peak);
      EXPECT_NEAR(*end, 0.5, 1e-9);
    } else {
      for (std::size_t index = 1U; index < mapped.size(); ++index) {
        EXPECT_GE(mapped[index], mapped[index - 1U]);
      }
      EXPECT_GT(*end, 0.525);
      EXPECT_LT(*end, 2.0);
      EXPECT_NEAR(mapped.back(), *end, 1e-9);
    }
  }
}

TEST(ParticipantStopStageHelperTest, InexactContractScopeOrCoordinateRefusesBeforeEffect) {
  auto wrong_resource = Parameters("soft_on_path");
  std::get<JsonObjectDTO>(
      std::get<JsonValueDTO::Array>(wrong_resource.at("per_resource_limits").value)
          .front()
          .value)
      .insert_or_assign("resource_id", JsonValueDTO{std::string("robot.joint.9")});
  auto linear = Parameters("soft_on_path");
  std::get<JsonObjectDTO>(
      std::get<JsonObjectDTO>(
          std::get<JsonValueDTO::Array>(linear.at("per_resource_limits").value).front().value)
          .at("max_deceleration")
          .value)
      .insert_or_assign("coordinate_kind", JsonValueDTO{std::string("linear_m_s2")});
  for (const auto& [parameters, reason] :
       {std::pair{wrong_resource, "motion_controlled_stop_resource_limit_invalid"},
        std::pair{linear, "motion_controlled_stop_deceleration_invalid"}}) {
    SCOPED_TRACE(reason);
    World world;
    const auto stage = Stage(0U, Nominal::kControlled, parameters);
    world.Dispatch(stage);
    world.Sample(0.5, 0.5, 1.0);
    const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
    EXPECT_EQ(result.attempt_disposition, StopStageAttemptDispositionDTO::kNotDispatched);
    EXPECT_EQ(result.error->details.at("reason"), reason);
    EXPECT_TRUE(world.port.commands().empty());
    EXPECT_EQ(world.freezes, 0U);
  }
}

TEST(ParticipantStopStageHelperTest, PlannerChosenHardStopIsTaggedForTheKernel) {
  // Infeasible deceleration and an initial-deviation trigger
  // both make the planner choose the hard stop.
  {
    World world;
    const auto stage =
        Stage(0U, Nominal::kControlled, Parameters("soft_on_path", false, std::nullopt, 0.01));
    world.Dispatch(stage);
    world.Sample(0.5, 0.5, 1.0);
    const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
    EXPECT_EQ(result.attempt_disposition, StopStageAttemptDispositionDTO::kNotDispatched);
    EXPECT_EQ(result.error->details.at("stop_disposition"), "planner_selected_hard_stop");
    EXPECT_EQ(result.error->details.at("recommended_disposition"), "hard_stop");
    EXPECT_TRUE(world.port.commands().empty());
  }
  World world(false);
  world.config.trip_authority = StateAuthorityClass::kMeasured;
  world.config.max_initial_path_deviation_rad = 0.01;
  world.Build();
  const auto stage = Stage(0U);
  world.Dispatch(stage);
  world.Sample(0.5, 0.5, 1.0, 0.5, -0.5);  // measured 0.5 rad before the path start
  const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
  EXPECT_EQ(result.error->details.at("reason"), "initial_path_deviation_exceeds_limit");
  EXPECT_EQ(result.error->details.at("stop_disposition"), "planner_selected_hard_stop");
}

// Real Lite 6: the measured position trails the
// command, so a lead-in anchored at the measured position can end before the
// replacement can start (the soft Stop is refused and falls back). The lane's
// `lead_in_anchor` selects the anchor; kCommanded (the default) starts the
// lead-in at the command cursor and the soft Stop is sent.
TEST(ParticipantStopStageHelperTest, MeasuredLaneLeadInAnchorSelectsTheSeam) {
  EXPECT_EQ(StopStageHelperConfig{}.lead_in_anchor, StopLeadInAnchor::kCommanded);
  for (const auto anchor : {StopLeadInAnchor::kCommanded, StopLeadInAnchor::kMeasured}) {
    const bool commanded = anchor == StopLeadInAnchor::kCommanded;
    SCOPED_TRACE(commanded ? "commanded" : "measured");
    World world(false);
    world.config.trip_authority = StateAuthorityClass::kMeasured;
    world.config.lead_in_anchor = anchor;
    world.Build();
    const auto stage = Stage(0U);
    world.Dispatch(stage);
    // Command cursor 0.5; the measured position trails it by 0.05 rad (q = t).
    // At the frozen factor 0.5 the lead-in ends 0.025 s of path after its
    // anchor, and the replacement starts >= 0.01 s of path after the cursor.
    world.Sample(0.5, 0.5, 1.0, 0.5, 0.45);
    if (commanded) ScriptReplacement(world, kSecond, 5U, ExecutionGoalResult::kSucceeded);
    const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
    if (commanded) {
      ASSERT_EQ(Replaces(world.port).size(), 1U);
      EXPECT_EQ(result.attempt_disposition, StopStageAttemptDispositionDTO::kDispatched);
      EXPECT_EQ(result.actual_provider_contract_mode, MotionControlledStopMode::kSoftOnPath);
      // The joined replacement never steps back behind the command cursor.
      // Keep the copy alive: C++20 range-for does not extend the temporary.
      const auto replaces = Replaces(world.port);
      for (const auto& point : replaces.front().trajectory.waypoints) {
        if (point.time_from_start_s >= 0.0) {
          EXPECT_GE(point.positions_rad.front(), 0.5 - 1e-9);
        }
      }
    } else {
      EXPECT_TRUE(Replaces(world.port).empty());
      EXPECT_EQ(result.attempt_disposition, StopStageAttemptDispositionDTO::kNotDispatched);
      ASSERT_TRUE(result.error);
      const auto& details = result.error->details;
      EXPECT_TRUE(std::any_of(details.begin(), details.end(), [](const auto& row) {
        return row.second.find("stop_delta_budget_exceeded") != std::string::npos;
      })) << result.error->message;
    }
  }
}

TEST(ParticipantStopStageHelperTest, RealLaneGoalToleranceViolationIsMainsUnsettledCase) {
  for (const auto tolerance :
       {ExecutionGoalTolerancePolicy::kKept, ExecutionGoalTolerancePolicy::kErased}) {
    World world;
    world.Lane(tolerance);
    const auto stage = Stage(0U);
    world.Dispatch(stage);
    world.Sample(0.5, 0.5, 1.0);
    ScriptReplacement(world, kSecond, 5U, ExecutionGoalResult::kAbortedGoalTolerance);
    const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
    EXPECT_EQ(result.attempt_disposition, StopStageAttemptDispositionDTO::kDispatched);
    EXPECT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kFailed);
    EXPECT_EQ(result.actual_provider_contract_mode, MotionControlledStopMode::kSoftOnPath);
    // The Kernel maps this reason to InterventionRequired with no second
    // Stop command; only a lane that keeps goal tolerances (every ros2_control
    // lane, merge 6 arrival) reports it.
    EXPECT_EQ(result.error->details.at("reason"),
              tolerance == ExecutionGoalTolerancePolicy::kKept
                  ? "stop_goal_tolerance_violated"
                  : "controller_terminal_result_unsuccessful");
    EXPECT_TRUE(Cancels(world.port).empty());
  }
}

// A Stop trajectory gets the lane's arrival check like a
// motion. On a lane that keeps goal tolerances, a controlled Stop goal (soft
// ramp, spring-back, fast spring-back) that JTC finishes within it succeeds;
// one that JTC aborts on the goal tolerance takes the existing path
// (`stop_goal_tolerance_violated`: the Kernel ends the participant
// InterventionRequired with no second Stop command).
TEST(ParticipantStopStageHelperTest, StopTrajectoriesTakeTheLaneArrivalCheckLikeMotions) {
  for (const auto* mode : {"soft_on_path", "spring_back_hold", "spring_back_fast"}) {
    for (const auto ended :
         {ExecutionGoalResult::kSucceeded, ExecutionGoalResult::kAbortedGoalTolerance}) {
      SCOPED_TRACE(std::string(mode) + " " + std::string(to_string(ended)));
      World world;
      world.Lane(ExecutionGoalTolerancePolicy::kKept);
      const auto stage = Stage(0U, Nominal::kControlled, Parameters(mode, false));
      world.Dispatch(stage);
      world.Sample(0.5, 0.5, 1.0);
      ScriptReplacement(world, kSecond, 5U, ended);
      const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
      EXPECT_EQ(result.attempt_disposition, StopStageAttemptDispositionDTO::kDispatched);
      ASSERT_EQ(Replaces(world.port).size(), 1U);
      EXPECT_TRUE(Cancels(world.port).empty());
      if (ended == ExecutionGoalResult::kSucceeded) {
        ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
            << result.error->details.at("reason");
        continue;
      }
      EXPECT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kFailed);
      ASSERT_TRUE(result.error.has_value());
      EXPECT_EQ(result.error->details.at("reason"), "stop_goal_tolerance_violated");
    }
  }
}

TEST(ParticipantStopStageHelperTest, PathDeviationDuringTheStopIsMeasuredFromSamples) {
  World world;
  const auto stage = Stage(0U);
  world.Dispatch(stage);
  world.Sample(0.5, 0.5, 1.0);
  world.on_tick = [&] {
    if (Replaces(world.port).empty()) return;
    world.Executing(kSecond);
    world.Sample(0.02, 0.55, 0.0);
    if (world.ticks == 3U) {
      ExecutionControllerSampleDTO off = *world.port.ReadSample();
      off.measured_positions_rad = {-0.5};  // 0.5 rad off the path start
      world.port.SetSample(off);
    }
    if (world.ticks >= 4U) world.Finish(kSecond, ExecutionGoalResult::kSucceeded);
  };
  const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
  ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded);
  const auto& evidence = EvidenceObject(result);
  EXPECT_NEAR(std::get<double>(evidence.at("path_deviation_max_rad").value), 0.5, 1e-6);
}

// --- Hard stop: level 3 and level 4 ----------------------------------------------

/// Brake installation and finish as the mechanism would report them.
/// Scripts JTC executing the level-3 brake as characterized: the
/// cursor restarts on the brake; the brake's own sampled reference stays
/// nonzero; on tick `succeeded_tick` JTC reports kSucceeded and from the next
/// tick on publishes its success hold (cursor reset, reference velocity 0).
void ScriptBrake(World& world, const ExecutionGoalKey& key, std::size_t succeeded_tick = 4U,
                 double factor = 0.5) {
  auto started = std::make_shared<std::size_t>(0U);
  world.on_tick = [&world, key, succeeded_tick, started, factor] {
    const auto replaces = Replaces(world.port);
    if (replaces.empty() || replaces.back().next_key != key) return;
    if (*started == 0U) {
      *started = world.ticks;
      world.Executing(key);
      world.port.FinishGoal(replaces.back().active_key, ExecutionGoalResult::kPreemptedBy, key,
                            world.clock.now());
    }
    const auto tick = world.ticks - *started + 1U;
    if (tick <= succeeded_tick) {
      world.Sample(0.004 * static_cast<double>(tick), 0.6, 0.04, factor);  // braking, never 0
      if (tick == succeeded_tick) world.Finish(key, ExecutionGoalResult::kSucceeded);
    } else {
      world.Sample(0.0, world.BrakeEnd(), 0.0, factor);  // success hold at the brake's end
    }
  };
}

TEST(ParticipantStopStageHelperTest, ExhaustedControlledFallbacksLeaveTheExistingHardStage) {
  World world;
  const auto stage =
      Stage(0U, Nominal::kControlled,
            FallbackParameters("soft_on_path", {"spring_back_hold", "spring_back_fast"}, 0.01));
  world.Dispatch(stage);
  world.Sample(0.5, 0.5, 1.0);
  world.port.QueueCommandResult(
      {ExecutionPortDispatch::kNotDispatched, ExecutionPortRefusal::kNotReady, std::nullopt});
  const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
  ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kFailed);
  EXPECT_EQ(result.attempt_disposition, StopStageAttemptDispositionDTO::kNotDispatched);
  ASSERT_TRUE(result.error);
  EXPECT_EQ(result.error->details.at("recommended_disposition"), "hard_stop");
  EXPECT_TRUE(result.error->details.at("fallback_attempt_1").starts_with("soft_on_path:"));
  EXPECT_TRUE(result.error->details.at("fallback_attempt_2").starts_with("spring_back_hold:"));
  EXPECT_TRUE(result.error->details.at("fallback_attempt_3").starts_with("spring_back_fast:"));
  EXPECT_EQ(Replaces(world.port).size(), 1U);
  EXPECT_EQ(world.freezes, 1U);
  EXPECT_EQ(stage.participant_stop_plan.stage_commands[1].resolved_stage.stage_kind,
            StopEscalationStageKind::kSoftwareHard);
  auto hard = stage;
  hard.stage_index = 1U;
  world.clock.advance(1);
  world.Sample(0.5, 0.5, 1.0);
  ScriptBrake(world, kSecond);
  const auto contained = world.helper->TranslateStopStage(hard, Budget(hard), {});
  ASSERT_EQ(contained.outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
      << contained.error->message;
  ASSERT_EQ(Replaces(world.port).size(), 2U);
  EXPECT_EQ(Replaces(world.port).back().next_key, kSecond);
  EXPECT_TRUE(Cancels(world.port).empty());
}

TEST(ParticipantStopStageHelperTest, ControlledHardStopBrakesFromTheLatestCommandedState) {
  World world;
  const auto stage = Stage(1U);
  world.Dispatch(stage);
  world.Sample(0.5, 0.5, 1.0);
  ScriptBrake(world, kSecond);
  const auto result = world.helper->TranslateStopStage(stage, Budget(stage, 1'800'000'000), {});
  ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
      << result.error->details.at("reason");
  EXPECT_FALSE(result.actual_provider_contract_mode.has_value());
  EXPECT_EQ(Scales(world.port), std::vector<double>{0.5});  // factor pin, once
  EXPECT_TRUE(Cancels(world.port).empty());
  const auto replaces = Replaces(world.port);
  ASSERT_EQ(replaces.size(), 1U);
  const auto& points = replaces.front().trajectory.waypoints;
  // Join before the brake; the brake ends at rest after 0.5 * 1 / 4 s.
  EXPECT_LT(points.front().time_from_start_s, 0.0);
  EXPECT_EQ(points.back().velocities_rad_s[0], 0.0);
  EXPECT_NEAR(points.back().time_from_start_s, 0.5 * 0.125 - 0.5 * 0.02, 1e-9);
  EXPECT_EQ(Text(EvidenceObject(result), "hard_stop_level"), "controlled_hard_stop");
}

TEST(ParticipantStopStageHelperTest, ScheduledHardStopUsesTheSameSourceEntryAndFullBrake) {
  World world;
  auto lane = world.port.ReadLaneFacts();
  lane.supports_scheduled_replacement = true;
  world.port.SetLaneFacts(lane);
  const auto stage = Stage(1U);
  world.Dispatch(stage);
  world.Sample(0.5, 0.5, 1.0);
  const auto source = world.port.ReadSample();
  ASSERT_TRUE(source);
  ScriptBrake(world, kSecond);
  const auto result = world.helper->TranslateStopStage(stage, Budget(stage, 1'800'000'000), {});
  ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded);
  const auto replacements = Replaces(world.port);
  ASSERT_EQ(replacements.size(), 1U);
  ASSERT_TRUE(replacements.front().start);
  EXPECT_EQ(replacements.front().start->experiment->release_hold, ExecutionReleaseHold::kHeld);
  const auto& points = replacements.front().trajectory.waypoints;
  EXPECT_NEAR(points.front().time_from_start_s, 0.004, 1e-12);
  EXPECT_NEAR(points.front().positions_rad[0], 0.514, 1e-12);
  EXPECT_NEAR(points.front().velocities_rad_s[0], 1.0, 1e-12);
  EXPECT_NEAR(points.front().accelerations_rad_s2[0], -16.0, 1e-12);
  EXPECT_NEAR(points.back().time_from_start_s, 0.004 + 0.5 * 0.125, 1e-12);
  EXPECT_EQ(Scales(world.port), std::vector<double>{0.5});
  EXPECT_TRUE(Cancels(world.port).empty());
}

TEST(ParticipantStopStageHelperTest, ControlledHardStopFinishesOnItsSuccessThenTheFollowingHold) {
  // Neither the brake's own reference nor a controller-wide hold
  // finishes level 3; only this replacement's kSucceeded followed by a hold
  // sample at exactly zero reference velocity does.
  World world;
  const auto stage = Stage(1U);
  world.Dispatch(stage);
  world.Sample(0.5, 0.5, 1.0);
  std::size_t started = 0U;
  std::size_t succeeded_at = 0U;
  world.on_tick = [&] {
    const auto replaces = Replaces(world.port);
    if (replaces.empty()) return;
    if (started == 0U) {
      started = world.ticks;
      world.Executing(kSecond);
      world.port.FinishGoal(kOriginal, ExecutionGoalResult::kPreemptedBy, kSecond,
                            world.clock.now());
    }
    const auto tick = world.ticks - started + 1U;
    if (tick <= 3U) {
      world.Sample(0.004 * static_cast<double>(tick), 0.6, 0.02);
    } else if (tick <= 6U) {
      // A zero reference before this replacement's result is not the finish.
      world.Sample(0.0, world.BrakeEnd(), 0.0);
    } else if (tick == 7U) {
      world.Finish(kSecond, ExecutionGoalResult::kSucceeded);  // no new sample yet
      succeeded_at = world.ticks;
    } else {
      world.Sample(0.0, world.BrakeEnd(), 0.0);
    }
  };
  const auto result = world.helper->TranslateStopStage(stage, Budget(stage, 1'800'000'000), {});
  ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
      << result.error->details.at("reason");
  ASSERT_GT(succeeded_at, 0U);
  EXPECT_GT(world.ticks, succeeded_at);
  const auto& evidence = EvidenceObject(result);
  EXPECT_EQ(Text(evidence, "hard_stop_level"), "controlled_hard_stop");
  EXPECT_EQ(Text(std::get<JsonObjectDTO>(evidence.at("post_command_sample").value),
                 "finish_basis"),
            "controlled_hard_stop_succeeded_then_hold");
  EXPECT_TRUE(Cancels(world.port).empty());

  // Without this replacement's success the hold never finishes it: as a sent
  // trajectory with the arrival check, the brake's no-report bound is a normal
  // motion's (max(10 s, 1.5 x duration + 2 s) from its send), not the
  // lane's 1 s stop-completion timeout.
  World held;
  const auto again = Stage(1U);
  held.Dispatch(again);
  held.Sample(0.5, 0.5, 1.0);
  held.on_tick = [&] {
    const auto replaces = Replaces(held.port);
    if (replaces.empty()) return;
    held.Executing(kSecond);
    held.Sample(0.0, 0.6, 0.0);
  };
  const auto started_at = held.clock.now();
  const auto failed = held.helper->TranslateStopStage(again, Budget(again, 0), {});
  EXPECT_EQ(failed.outcome, ParticipantStopStageOutcomeDTO::kFailed);
  EXPECT_EQ(failed.error->details.at("reason"), "stop_evidence_phase_expired");
  EXPECT_GE(held.clock.now() - started_at, 10'000'000'000);
  EXPECT_LE(held.clock.now() - started_at, 10'000'000'000 + kTick);
}

// The brake finishes on the controller's report even after the lane's
// hard-stop timeout (here 1 s; e.g. Isaac UR10e's 0.5 s below its 1.0 s
// goal_time). Within tolerance it succeeds; a goal-tolerance abort falls to
// the floor, which then has the lane's hard-stop timeout of its own.
TEST(ParticipantStopStageHelperTest, BrakeFinishesOnTheControllerReportPastTheHardStopTimeout) {
  for (const auto ended :
       {ExecutionGoalResult::kSucceeded, ExecutionGoalResult::kAbortedGoalTolerance}) {
    SCOPED_TRACE(to_string(ended));
    World world;
    world.Lane(ExecutionGoalTolerancePolicy::kKept);
    const auto stage = Stage(1U);
    world.Dispatch(stage);
    world.Sample(0.5, 0.5, 1.0);
    const auto t0 = world.clock.now();
    bool installed = false;
    std::optional<TimePoint> ended_at;
    world.on_tick = [&] {
      if (Replaces(world.port).empty()) return;
      if (!installed) {
        installed = true;
        world.Executing(kSecond);
        world.port.FinishGoal(kOriginal, ExecutionGoalResult::kPreemptedBy, kSecond,
                              world.clock.now());
        world.Sample(0.004, 0.55, 0.4);  // installed: the cursor restarted
        return;
      }
      // The controller's verdict comes 1.5 s after the stage started.
      if (!ended_at && world.clock.now() >= t0 + 1'500'000'000) {
        world.Finish(kSecond, ended);
        ended_at = world.clock.now();
        return;
      }
      // The brake's reference rests at its end; after an abort JTC holds.
      world.Sample(0.0, world.BrakeEnd(), 0.0);
    };
    const auto result = world.helper->TranslateStopStage(stage, Budget(stage, 1'800'000'000), {});
    ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
        << result.error->details.at("reason");
    ASSERT_TRUE(ended_at.has_value());
    EXPECT_GT(world.clock.now() - t0, world.config.hard_stop_completion_timeout_ns);
    EXPECT_TRUE(Cancels(world.port).empty());
    const auto& evidence = EvidenceObject(result);
    if (ended == ExecutionGoalResult::kSucceeded) {
      EXPECT_EQ(Text(evidence, "hard_stop_level"), "controlled_hard_stop");
    } else {
      EXPECT_EQ(Text(evidence, "hard_stop_level"), "goal_already_terminal");
      EXPECT_EQ(Text(evidence, "hard_stop_fallback_reason"),
                "controlled_hard_stop_ended_aborted_goal_tolerance");
      EXPECT_LE(world.clock.now() - *ended_at, world.config.hard_stop_completion_timeout_ns);
    }
  }
}

TEST(ParticipantStopStageHelperTest, BrakeEndingOtherwiseThanSucceededFallsToTheFloorAtOnce) {
  // A level-3 brake that ends aborted, preempted or unknown is not
  // finished; level 4 follows at once (nothing is left to cancel, so the
  // floor observes the controller's hold) instead of waiting for the bound.
  // A brake outside the lane's arrival tolerance after `goal_time` is
  // JTC's goal-tolerance abort and takes the same path.
  for (const auto ended : {ExecutionGoalResult::kAbortedPathTolerance,
                           ExecutionGoalResult::kAbortedGoalTolerance,
                           ExecutionGoalResult::kEndedUnknown, ExecutionGoalResult::kPreemptedBy}) {
    SCOPED_TRACE(to_string(ended));
    World world;
    const auto stage = Stage(1U);
    world.Dispatch(stage);
    world.Sample(0.5, 0.5, 1.0);
    std::size_t started = 0U;
    world.on_tick = [&] {
      if (Replaces(world.port).empty()) return;
      if (started == 0U) {
        started = world.ticks;
        world.Executing(kSecond);
        world.port.FinishGoal(kOriginal, ExecutionGoalResult::kPreemptedBy, kSecond,
                              world.clock.now());
        world.Sample(0.004, 0.55, 0.4);  // installed: the cursor restarted
      } else if (world.ticks == started + 1U) {
        world.Finish(kSecond, ended);
      } else {
        world.Sample(0.0, 0.56, 0.0);  // the controller holds
      }
    };
    const auto result = world.helper->TranslateStopStage(stage, Budget(stage, 1'800'000'000), {});
    ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
        << result.error->details.at("reason");
    const auto& evidence = EvidenceObject(result);
    EXPECT_EQ(Text(evidence, "hard_stop_level"), "goal_already_terminal");
    EXPECT_EQ(Text(evidence, "hard_stop_fallback_reason"),
              std::string("controlled_hard_stop_ended_") + to_string(ended));
    EXPECT_TRUE(Cancels(world.port).empty());  // nothing left that may run
    EXPECT_LT(world.ticks, started + 10U);     // far inside the 1 s bound (100 ticks)
  }
}

TEST(ParticipantStopStageHelperTest, RefusedOrUninstalledBrakeFallsBackToTheCancel) {
  {
    World world;
    const auto stage = Stage(1U);
    world.Dispatch(stage);
    world.Sample(0.5, 0.5, 1.0);
    world.port.QueueCommandResult({ExecutionPortDispatch::kDispatched, std::nullopt, std::nullopt});
    world.port.QueueCommandResult({ExecutionPortDispatch::kNotDispatched,
                                   ExecutionPortRefusal::kInvalidInput, std::nullopt});
    world.on_tick = [&] {
      if (Cancels(world.port).empty()) return;
      world.port.MarkCancelSent(kOriginal, world.clock.now() - 1);
      world.Sample(0.001, 0.52, world.ticks >= 3U ? 0.0 : 0.3);  // cancel trajectory
      if (world.ticks == 2U) world.Cancelled(kOriginal);
    };
    const auto result = world.helper->TranslateStopStage(stage, Budget(stage, 1'800'000'000), {});
    ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
        << result.error->details.at("reason");
    EXPECT_EQ(Cancels(world.port), std::vector<ExecutionGoalKey>{kOriginal});
    const auto& evidence = EvidenceObject(result);
    EXPECT_EQ(Text(evidence, "hard_stop_level"), "jtc_cancel_decelerate_on_cancel");
    EXPECT_EQ(Text(evidence, "hard_stop_fallback_reason"), "controlled_hard_stop_refused");
    EXPECT_EQ(Text(std::get<JsonObjectDTO>(evidence.at("post_command_sample").value),
                   "finish_basis"),
              "cancel_reference_at_rest");
  }
  World world;
  const auto stage = Stage(1U);
  world.Dispatch(stage);
  world.Sample(0.5, 0.5, 1.0);
  world.on_tick = [&] {
    if (Cancels(world.port).empty()) return;  // the brake never shows up
    world.port.MarkCancelSent(kSecond, world.clock.now() - 1);
    world.Sample(0.001, 0.52, 0.0);
    world.Cancelled(kSecond);
  };
  const auto result = world.helper->TranslateStopStage(stage, Budget(stage, 1'800'000'000), {});
  ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
      << result.error->details.at("reason");
  // Both goals that may run are cancelled, newest first.
  EXPECT_EQ(Cancels(world.port), (std::vector<ExecutionGoalKey>{kSecond, kOriginal}));
  EXPECT_EQ(Text(EvidenceObject(result), "hard_stop_fallback_reason"),
            "controlled_hard_stop_not_installed");
}

// --- Takeover proof within the lane lead-in; stale controller state -----------

TEST(ParticipantStopStageHelperTest, BrakeRecognisedWithinTheLeadInNeedsNoFallback) {
  // The brake's own first feedback at ~50 ms is inside a 100 ms lead-in
  // (`stop_traj_delta_s`); the 20 ms replacement start allowance is only the
  // join estimate and is not the takeover window.
  World world(false);
  world.config.stop_traj_delta_s = 0.1;
  world.Build();
  const auto stage = Stage(1U);
  world.Dispatch(stage);
  world.Sample(0.5, 0.5, 1.0);
  std::size_t sent_tick = 0U;
  std::size_t feedback_tick = 0U;
  world.on_tick = [&] {
    const auto replaces = Replaces(world.port);
    if (replaces.empty()) return;
    if (sent_tick == 0U) sent_tick = world.ticks;
    const auto since = world.ticks - sent_tick;
    if (since < 5U) {
      // Accepted (and running) without its own feedback: never proof.
      world.port.SetGoalPhase(kSecond, ExecutionGoalPhase::kExecuting);
      return;
    }
    if (feedback_tick == 0U) {
      feedback_tick = world.ticks;
      world.Executing(kSecond);
      world.port.FinishGoal(kOriginal, ExecutionGoalResult::kPreemptedBy, kSecond,
                            world.clock.now());
    }
    const auto tick = world.ticks - feedback_tick + 1U;
    if (tick <= 4U) {
      world.Sample(0.004 * static_cast<double>(tick), 0.6, 0.04);
      if (tick == 4U) world.Finish(kSecond, ExecutionGoalResult::kSucceeded);
    } else {
      world.Sample(0.0, world.BrakeEnd(), 0.0);
    }
  };
  const auto result = world.helper->TranslateStopStage(stage, Budget(stage, 1'800'000'000), {});
  ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
      << result.error->details.at("reason");
  const auto& evidence = EvidenceObject(result);
  EXPECT_EQ(Text(evidence, "hard_stop_level"), "controlled_hard_stop");
  EXPECT_FALSE(evidence.contains("hard_stop_fallback_reason"));
  EXPECT_TRUE(Cancels(world.port).empty());
  ASSERT_NE(sent_tick, 0U);
  EXPECT_EQ(feedback_tick - sent_tick, 5U);  // ~50 ms after the brake was sent
}

TEST(ParticipantStopStageHelperTest, BrakeWithoutOwnFeedbackWithinTheLeadInFallsBackToCancel) {
  // Level 3 brakes at once; with no own feedback or result within the
  // lead-in (acceptance alone is never proof) the cancel follows.
  World world(false);
  world.config.stop_traj_delta_s = 0.1;
  world.Build();
  const auto stage = Stage(1U);
  world.Dispatch(stage);
  world.Sample(0.5, 0.5, 1.0);
  std::size_t sent_tick = 0U;
  std::size_t cancel_tick = 0U;
  world.on_tick = [&] {
    if (Replaces(world.port).empty()) return;
    if (sent_tick == 0U) sent_tick = world.ticks;
    if (Cancels(world.port).empty()) {
      world.port.SetGoalPhase(kSecond, ExecutionGoalPhase::kExecuting);  // accepted only
      world.Sample(0.5, 0.5, 1.0);
      return;
    }
    if (cancel_tick == 0U) cancel_tick = world.ticks;
    world.port.MarkCancelSent(kSecond, world.clock.now() - 1);
    world.Sample(0.001, 0.52, 0.0);
    world.Cancelled(kSecond);
  };
  const auto result = world.helper->TranslateStopStage(stage, Budget(stage, 1'800'000'000), {});
  ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
      << result.error->details.at("reason");
  EXPECT_EQ(Cancels(world.port), (std::vector<ExecutionGoalKey>{kSecond, kOriginal}));
  EXPECT_EQ(Text(EvidenceObject(result), "hard_stop_fallback_reason"),
            "controlled_hard_stop_not_installed");
  ASSERT_NE(sent_tick, 0U);
  ASSERT_NE(cancel_tick, 0U);
  // The cancel follows at the end of the 100 ms lead-in, not after the 20 ms
  // replacement start allowance.
  EXPECT_GE(cancel_tick - sent_tick, 9U);
  EXPECT_LE(cancel_tick - sent_tick, 11U);
}

/// The brake's own first feedback is received at `receipt_after`
/// (local receipt, after the brake was sent) while the helper's poll only
/// wakes at `poll_after`; afterwards the brake runs and holds like JTC.
/// `preempts`: whether the controller already reports the original goal
/// preempted by the brake (else the original still counts as live).
void ScriptLateWakeBrake(World& world, const Duration receipt_after, const Duration poll_after,
                         const bool preempts = true) {
  auto state = std::make_shared<std::pair<TimePoint, std::size_t>>(0, 0U);
  world.on_tick = [&world, receipt_after, poll_after, preempts, state] {
    if (Replaces(world.port).empty()) return;
    if (!Cancels(world.port).empty()) {
      world.port.MarkCancelSent(kSecond, world.clock.now() - 1);
      world.Sample(0.001, 0.52, 0.0);
      world.Cancelled(kSecond);
      return;
    }
    auto& [sent_at, tick] = *state;
    if (sent_at == 0) {
      // The brake was sent one waiter step (kTick) before this first tick.
      sent_at = world.clock.now() - kTick;
      world.clock.advance(sent_at + poll_after - world.clock.now());
      ExecutionGoalObservationDTO goal;
      goal.key = kSecond;
      goal.binding_epoch = ExecutionBindingEpoch(1U);
      goal.phase = ExecutionGoalPhase::kExecuting;
      goal.mechanism_goal_id = MechanismGoalId("uuid-2");
      ExecutionGoalFeedbackDTO feedback;
      feedback.source_stamp_ns = sent_at + receipt_after;
      feedback.local_receipt_stamp_ns = sent_at + receipt_after;
      goal.latest_feedback = feedback;
      world.port.SetGoal(goal);
      if (preempts) {
        world.port.FinishGoal(kOriginal, ExecutionGoalResult::kPreemptedBy, kSecond,
                              world.clock.now());
      }
      return;
    }
    ++tick;
    if (tick <= 4U) {
      world.Sample(0.004 * static_cast<double>(tick), 0.6, 0.04);
      if (tick == 4U) world.Finish(kSecond, ExecutionGoalResult::kSucceeded);
    } else {
      world.Sample(0.0, world.BrakeEnd(), 0.0);
    }
  };
}

TEST(ParticipantStopStageHelperTest, BrakeFeedbackReceivedAfterTheLeadInFallsBackToCancel) {
  // Own feedback received at 110 ms with a 100 ms lead-in is not
  // proof of takeover, even though the poll finds it; the cancel follows.
  World world(false);
  world.config.stop_traj_delta_s = 0.1;
  world.Build();
  const auto stage = Stage(1U);
  world.Dispatch(stage);
  world.Sample(0.5, 0.5, 1.0);
  ScriptLateWakeBrake(world, 110'000'000, 110'000'000, false);
  const auto result = world.helper->TranslateStopStage(stage, Budget(stage, 1'800'000'000), {});
  ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
      << result.error->details.at("reason");
  EXPECT_EQ(Text(EvidenceObject(result), "hard_stop_fallback_reason"),
            "controlled_hard_stop_not_installed");
  EXPECT_EQ(Cancels(world.port), (std::vector<ExecutionGoalKey>{kSecond, kOriginal}));
}

TEST(ParticipantStopStageHelperTest, BrakeFeedbackReceivedInsideTheLeadInCountsWhenThePollWakesLate) {
  // Own feedback received at 90 ms (lead-in 100 ms) is proof even
  // though the helper's poll only wakes at 110 ms; no fallback.
  World world(false);
  world.config.stop_traj_delta_s = 0.1;
  world.Build();
  const auto stage = Stage(1U);
  world.Dispatch(stage);
  world.Sample(0.5, 0.5, 1.0);
  ScriptLateWakeBrake(world, 90'000'000, 110'000'000);
  const auto result = world.helper->TranslateStopStage(stage, Budget(stage, 1'800'000'000), {});
  ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
      << result.error->details.at("reason");
  const auto& evidence = EvidenceObject(result);
  EXPECT_EQ(Text(evidence, "hard_stop_level"), "controlled_hard_stop");
  EXPECT_FALSE(evidence.contains("hard_stop_fallback_reason"));
  EXPECT_TRUE(Cancels(world.port).empty());
}

TEST(ParticipantStopStageHelperTest, BrakeProofCountsOnlyInsideTheStageCappedRecognitionWindow) {
  // The stage's remaining bound (50 ms) is shorter than the lead-in
  // (100 ms), so the recognition window is capped at 50 ms. Proof received at
  // 80 ms is not eligible (the cancel follows); proof received at 40 ms is
  // eligible even though the poll only wakes at 80 ms.
  for (const Duration receipt_after : {Duration{80'000'000}, Duration{40'000'000}}) {
    SCOPED_TRACE(receipt_after);
    World world(false);
    world.config.stop_traj_delta_s = 0.1;
    world.config.hard_stop_completion_timeout_ns = 50'000'000;
    world.Build();
    const auto stage = Stage(1U);
    world.Dispatch(stage);
    world.Sample(0.5, 0.5, 1.0);
    ScriptLateWakeBrake(world, receipt_after, 80'000'000, false);
    const auto result =
        world.helper->TranslateStopStage(stage, Budget(stage, 1'800'000'000), {});
    // The 50 ms bound has passed when the poll wakes at 80 ms. Not eligible:
    // level 4 follows within that passed bound and fails. Eligible: the brake
    // took over and, as a sent trajectory, finishes on its own report under a
    // normal motion's bound, not the 50 ms.
    if (receipt_after > 50'000'000) {
      ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kFailed);
      ASSERT_TRUE(result.error.has_value());
      EXPECT_FALSE(Cancels(world.port).empty());  // not eligible: level 4 follows
      EXPECT_EQ(result.error->details.at("hard_stop_level"), "jtc_cancel_decelerate_on_cancel");
    } else {
      ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
          << result.error->details.at("reason");
      EXPECT_TRUE(Cancels(world.port).empty());  // eligible: the brake took over
      EXPECT_EQ(Text(EvidenceObject(result), "hard_stop_level"), "controlled_hard_stop");
    }
  }
}

TEST(ParticipantStopStageHelperTest, ReplacementFeedbackReceivedAfterTheLeadInIsNoTakeover) {
  // On the controlled replacement: own feedback received at 110 ms
  // with a 100 ms lead-in is not proof; the stage recommends the hard stop.
  for (const Duration receipt_after : {Duration{110'000'000}, Duration{90'000'000}}) {
    SCOPED_TRACE(receipt_after);
    World world(false);
    world.config.stop_traj_delta_s = 0.1;
    world.Build();
    const auto stage = Stage(0U);
    world.Dispatch(stage);
    world.Sample(0.5, 0.5, 1.0);
    TimePoint sent_at = 0;
    std::size_t tick = 0U;
    world.on_tick = [&] {
      if (Replaces(world.port).empty()) return;
      if (sent_at == 0) {
        sent_at = world.clock.now() - kTick;
        world.clock.advance(sent_at + 110'000'000 - world.clock.now());
        ExecutionGoalObservationDTO goal;
        goal.key = kSecond;
        goal.binding_epoch = ExecutionBindingEpoch(1U);
        goal.phase = ExecutionGoalPhase::kExecuting;
        goal.mechanism_goal_id = MechanismGoalId("uuid-2");
        ExecutionGoalFeedbackDTO feedback;
        feedback.source_stamp_ns = sent_at + receipt_after;
        feedback.local_receipt_stamp_ns = sent_at + receipt_after;
        goal.latest_feedback = feedback;
        world.port.SetGoal(goal);
        world.port.FinishGoal(kOriginal, ExecutionGoalResult::kPreemptedBy, kSecond,
                              world.clock.now());
        return;
      }
      ++tick;
      world.Sample(0.01 * static_cast<double>(tick), 0.5, tick >= 3U ? 0.0 : 0.5);
      if (tick == 3U) world.Finish(kSecond, ExecutionGoalResult::kSucceeded);
    };
    const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
    if (receipt_after > 100'000'000) {
      ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kFailed);
      EXPECT_EQ(result.error->details.at("reason"), "soft_on_path_stop_delta_budget_exceeded");
      EXPECT_EQ(Cancels(world.port), std::vector<ExecutionGoalKey>{kSecond});
    } else {
      ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
          << result.error->details.at("reason");
    }
  }
}

TEST(ParticipantStopStageHelperTest, StaleControllerStateDuringAStopIsAFault) {
  // As `main`'s `joint_state_max_age_s`: the controller stops reporting
  // state during the Stop; after the lane's 250 ms maximum age the stage
  // fails with stale controller state instead of waiting for its bound.
  World world(false);
  world.config.controller_state_max_age_ns = 250'000'000;
  world.Build();
  const auto stage = Stage(1U);
  world.Dispatch(stage);
  world.Sample(0.5, 0.5, 1.0);
  const auto t0 = world.clock.now();
  world.on_tick = [&] {
    // The brake runs (own feedback) but no controller sample arrives anymore.
    if (!Replaces(world.port).empty()) world.Executing(kSecond);
  };
  const auto result = world.helper->TranslateStopStage(stage, Budget(stage, 1'800'000'000), {});
  EXPECT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kFailed);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->details.at("reason"), "stale_controller_state");
  EXPECT_LE(world.clock.now() - t0, 250'000'000 + 2 * kTick);
  EXPECT_LT(world.clock.now() - t0, world.config.hard_stop_completion_timeout_ns);
}

TEST(ParticipantStopStageHelperTest, SampleReceivedBetweenTheClockAndSampleReadsIsNotStale) {
  // Stage F run 9 (same race as the participant's Evaluate): a sample whose
  // receipt lands on the same clock between the stale check's clock read and
  // its sample read is fresh (age 0), not stale with a negative age.
  World world(false);
  world.config.controller_state_max_age_ns = 250'000'000;
  world.Build();
  world.port.SetOnReadSample(
      [&world, last = std::uint64_t{0}](std::optional<ExecutionControllerSampleDTO>& sample) mutable {
        if (!sample || sample->local_receipt_count.value() == last) return;
        last = sample->local_receipt_count.value();
        world.clock.advance(1'000);
        sample->local_receipt_stamp_ns = world.clock.now();
      });
  const auto stage = Stage(1U);
  world.Dispatch(stage);
  world.Sample(0.5, 0.5, 1.0);
  std::size_t started = 0U;
  world.on_tick = [&] {
    const auto replaces = Replaces(world.port);
    if (replaces.empty()) return;
    if (started == 0U) {
      started = world.ticks;
      world.Executing(kSecond);
      world.port.FinishGoal(kOriginal, ExecutionGoalResult::kPreemptedBy, kSecond,
                            world.clock.now());
    }
    const auto tick = world.ticks - started + 1U;
    if (tick <= 3U) {
      world.Sample(0.004 * static_cast<double>(tick), 0.6, 0.02);
    } else if (tick == 4U) {
      world.Finish(kSecond, ExecutionGoalResult::kSucceeded);
    } else {
      world.Sample(0.0, world.BrakeEnd(), 0.0);
    }
  };
  const auto result = world.helper->TranslateStopStage(stage, Budget(stage, 1'800'000'000), {});
  world.port.SetOnReadSample(nullptr);
  ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
      << result.error->details.at("reason");
  EXPECT_TRUE(Cancels(world.port).empty());
}

TEST(ParticipantStopStageHelperTest, CancelAtFactorZeroFinishesOnTheAcknowledgement) {
  World world;
  const auto stage = Stage(1U);
  world.Dispatch(stage);
  world.Sample(0.5, 0.5, 1.0, 0.0);
  world.on_tick = [&] {
    if (Cancels(world.port).empty()) return;
    world.port.MarkCancelSent(kOriginal, world.clock.now() - 1);
    world.Sample(0.5, 0.5, 1.0, 0.0);  // frozen: no braking reference
    if (world.ticks >= 2U) world.port.AcknowledgeCancel(kOriginal, world.clock.now());
  };
  const auto result = world.helper->TranslateStopStage(stage, Budget(stage, 1'800'000'000), {});
  ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded);
  EXPECT_TRUE(Replaces(world.port).empty());  // no level 3 at factor 0
  EXPECT_EQ(Scales(world.port), std::vector<double>{0.0});
  EXPECT_EQ(Cancels(world.port), std::vector<ExecutionGoalKey>{kOriginal});  // also at 0
  const auto& evidence = EvidenceObject(result);
  EXPECT_EQ(Text(evidence, "hard_stop_fallback_reason"), "controlled_hard_stop_factor_zero");
  EXPECT_EQ(Text(std::get<JsonObjectDTO>(evidence.at("post_command_sample").value),
                 "finish_basis"),
            "factor_zero_cancel_acknowledged");
}

TEST(ParticipantStopStageHelperTest, CancelFinishIgnoresSamplesNotAssociatedWithTheCancel) {
  // An old zero sample, or one received before the cancel was sent, never
  // counts; without the finish the Stop deadline decides (no rest check).
  World world(false);
  world.config.max_deceleration_on_cancel_rad_s2.clear();  // level 3 unavailable
  world.Build();
  const auto stage = Stage(1U);
  world.Dispatch(stage);
  world.Sample(0.5, 0.5, 1.0);
  world.on_tick = [&] {
    if (Cancels(world.port).empty()) return;
    world.port.MarkCancelSent(kOriginal, world.clock.now() + 1'000'000'000);
    world.Sample(0.001, 0.5, 0.0);  // received before the cancel was sent
    world.Cancelled(kOriginal);
  };
  const auto result = world.helper->TranslateStopStage(stage, Budget(stage, 1'800'000'000), {});
  EXPECT_EQ(result.attempt_disposition, StopStageAttemptDispositionDTO::kDispatched);
  EXPECT_EQ(result.error->details.at("reason"), "stop_evidence_phase_expired");
  EXPECT_EQ(result.error->details.at("hard_stop_level"), "jtc_cancel_decelerate_on_cancel");

  World moving;
  const auto again = Stage(1U);
  moving.Dispatch(again);
  moving.Sample(0.5, 0.5, 1.0, 0.0);
  moving.on_tick = [&] {
    if (Cancels(moving.port).empty()) return;
    moving.port.MarkCancelSent(kOriginal, moving.clock.now() - 1);
    moving.Sample(0.6, 0.5, 0.0, 0.5);  // zero velocity but no switch observed
    moving.Cancelled(kOriginal);
  };
  const auto stuck = moving.helper->TranslateStopStage(again, Budget(again, 1'800'000'000), {});
  EXPECT_EQ(stuck.error->details.at("reason"), "stop_evidence_phase_expired");
}

TEST(ParticipantStopStageHelperTest, ProfileSelectedHardDecelerationIsTheCancelDirectly) {
  World world;
  const auto stage = Stage(0U, Nominal::kControlled, Parameters("hard_deceleration"));
  world.Dispatch(stage);
  world.Sample(0.5, 0.5, 1.0);
  world.on_tick = [&] {
    if (Cancels(world.port).empty()) return;
    world.port.MarkCancelSent(kOriginal, world.clock.now() - 1);
    world.Sample(0.001, 0.52, 0.0);
    world.Cancelled(kOriginal);
  };
  const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
  ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
      << result.error->details.at("reason");
  EXPECT_TRUE(Replaces(world.port).empty());
  EXPECT_EQ(result.actual_provider_contract_mode, MotionControlledStopMode::kHardDeceleration);
  EXPECT_EQ(Text(EvidenceObject(result), "stop_command_source"),
            "jtc_cancel_decelerate_on_cancel");
}

TEST(ParticipantStopStageHelperTest, HardStageAfterASoftReplacementBrakesTheReplacement) {
  World world;
  const auto soft = Stage(0U);
  world.Dispatch(soft);
  world.Sample(0.5, 0.5, 1.0);
  ScriptReplacement(world, kSecond, 1000U, std::nullopt);  // never finishes
  const auto first = world.helper->TranslateStopStage(soft, Budget(soft), {});
  ASSERT_EQ(first.error->details.at("reason"), "backend_stop_not_confirmed");
  const auto elapsed = world.clock.now() - 1'000'000'000;
  world.Sample(0.3, 0.6, 0.5);
  ScriptBrake(world, kThird);
  const auto hard = Stage(1U);
  const auto result = world.helper->TranslateStopStage(hard, Budget(hard, elapsed), {});
  ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
      << result.error->details.at("reason");
  const auto replaces = Replaces(world.port);
  ASSERT_EQ(replaces.size(), 2U);
  EXPECT_EQ(replaces.back().active_key, kSecond);
  EXPECT_EQ(replaces.back().next_key, kThird);
}

TEST(ParticipantStopStageHelperTest, HardStageObservesAnAlreadyEndedGoalWithoutACommand) {
  for (const auto ended : {ExecutionGoalResult::kSucceeded, ExecutionGoalResult::kCanceled,
                           ExecutionGoalResult::kAbortedPathTolerance}) {
    World world;
    const auto stage = Stage(1U);
    world.Dispatch(stage);
    world.Finish(kOriginal, ended);
    world.on_tick = [&] { world.Sample(2.0, 2.0, 0.0); };
    const auto result = world.helper->TranslateStopStage(stage, Budget(stage, 1'800'000'000), {});
    ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded);
    EXPECT_TRUE(world.port.commands().empty());
    EXPECT_EQ(Text(EvidenceObject(result), "hard_stop_level"), "goal_already_terminal");
  }
}

TEST(ParticipantStopStageHelperTest, FreshTerminalReadKeepsOldTransitionDataWithoutRewritingDtos) {
  for (const auto ended : {ExecutionGoalResult::kSucceeded, ExecutionGoalResult::kCanceled,
                           ExecutionGoalResult::kAbortedPathTolerance}) {
    for (const bool has_receipt : {false, true}) {
      SCOPED_TRACE(static_cast<int>(ended));
      SCOPED_TRACE(has_receipt);
      World world(false);
      GoalReadObservingPort observing(world.port);
      world.config.controller_state_max_age_ns = 250'000'000;
      world.Build(&observing);
      const auto stage = Stage(0U, Nominal::kGoalCancel);  // requires goal-terminal evidence
      world.Dispatch(stage);
      world.Finish(kOriginal, ended);
      if (!has_receipt) {
        auto goal = *world.port.ReadGoal(kOriginal);
        goal.terminal_local_receipt_stamp.reset();
        world.port.SetGoal(goal);
      }
      const auto original_goal = world.port.ReadGoal(kOriginal);
      world.clock.advance(500'000'000);  // transition is older than the 100 ms evidence budget
      world.Sample(2.0, 2.0, 1.0);
      // Readiness flags do not gate an already-ended Stop's evidence.
      world.port.SetReadiness({false, false, false, ExecutionBindingEpoch(1U)});
      std::optional<ExecutionControllerSampleDTO> hold_sample;
      world.on_tick = [&] {
        world.Sample(2.0, 2.0, 0.0);
        hold_sample = world.port.ReadSample();
      };
      observing.on_read_goal = [&](auto&) {
        if (world.ticks > 0U) {
          // The current observation must be stamped AFTER the actual read;
          // a timestamp taken before this call would itself exceed its budget.
          world.clock.advance(150'000'000);
        }
      };
      const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
      ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded);
      EXPECT_TRUE(world.port.commands().empty());
      ASSERT_EQ(result.evidence_items.size(), 1U);
      EXPECT_EQ(result.evidence_items.front().source_age_at_evaluation_ns, 0);
      const auto& goal_evidence =
          std::get<JsonObjectDTO>(EvidenceObject(result).at("goal_observation").value);
      EXPECT_EQ(Text(goal_evidence, "finish_basis"), "goal_already_terminal");
      if (has_receipt) {
        EXPECT_EQ(std::get<double>(goal_evidence.at("terminal_local_receipt_stamp_ns").value),
                  static_cast<double>(*original_goal->terminal_local_receipt_stamp));
        EXPECT_EQ(std::get<double>(goal_evidence.at("terminal_transition_age_ns").value),
                  static_cast<double>(world.clock.now() -
                                      *original_goal->terminal_local_receipt_stamp));
        EXPECT_GT(std::get<double>(goal_evidence.at("terminal_transition_age_ns").value),
                  100'000'000.0);
      } else {
        EXPECT_FALSE(goal_evidence.contains("terminal_local_receipt_stamp_ns"));
        EXPECT_FALSE(goal_evidence.contains("terminal_transition_age_ns"));
      }
      EXPECT_EQ(world.port.ReadGoal(kOriginal), original_goal);
      EXPECT_EQ(world.port.ReadSample(), hold_sample);
    }
  }
}

TEST(ParticipantStopStageHelperTest, OldTerminalFactDoesNotMakeAStaleControllerFeedFresh) {
  World world(false);
  world.config.controller_state_max_age_ns = 250'000'000;
  world.Build();
  const auto stage = Stage(0U, Nominal::kGoalCancel);
  world.Dispatch(stage);
  world.Finish(kOriginal, ExecutionGoalResult::kSucceeded);
  const auto original_goal = world.port.ReadGoal(kOriginal);
  world.Sample(2.0, 2.0, 0.0);
  const auto original_sample = world.port.ReadSample();
  world.clock.advance(500'000'000);
  const auto started = world.clock.now();
  const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
  ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kFailed);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->details.at("reason"), "stale_controller_state");
  EXPECT_EQ(world.clock.now(), started);
  EXPECT_TRUE(world.port.commands().empty());
  EXPECT_EQ(world.port.ReadGoal(kOriginal), original_goal);
  EXPECT_EQ(world.port.ReadSample(), original_sample);
}

TEST(ParticipantStopStageHelperTest, CurrentTerminalEvidenceRequiresTheExactGoalAndBinding) {
  enum class Mismatch { kGoalKey, kZeroGoalEpoch, kGoalEpoch, kCurrentEpoch, kSampleEpoch };
  for (const auto mismatch : {Mismatch::kGoalKey, Mismatch::kZeroGoalEpoch,
                              Mismatch::kGoalEpoch, Mismatch::kCurrentEpoch,
                              Mismatch::kSampleEpoch}) {
    SCOPED_TRACE(static_cast<int>(mismatch));
    World world(false);
    GoalReadObservingPort observing(world.port);
    world.Build(&observing);
    const auto stage = Stage(0U, Nominal::kGoalCancel);
    world.Dispatch(stage);
    world.Finish(kOriginal, ExecutionGoalResult::kSucceeded);
    const auto original_goal = world.port.ReadGoal(kOriginal);
    world.clock.advance(500'000'000);
    world.on_tick = [&] { world.Sample(2.0, 2.0, 0.0); };
    observing.on_read_goal = [&](auto& goal) {
      if (world.ticks == 0U || !goal.has_value()) return;
      switch (mismatch) {
        case Mismatch::kGoalKey:
          goal->key = kSecond;
          break;
        case Mismatch::kZeroGoalEpoch:
          goal->binding_epoch = ExecutionBindingEpoch(0U);
          break;
        case Mismatch::kGoalEpoch:
          goal->binding_epoch = ExecutionBindingEpoch(2U);
          break;
        case Mismatch::kCurrentEpoch:
          world.port.SetReadiness({true, true, true, ExecutionBindingEpoch(2U)});
          break;
        case Mismatch::kSampleEpoch:
          goal->binding_epoch = ExecutionBindingEpoch(2U);
          world.port.SetReadiness({true, true, true, ExecutionBindingEpoch(2U)});
          break;
      }
    };
    const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
    ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kFailed);
    ASSERT_TRUE(result.error.has_value());
    EXPECT_EQ(result.error->details.at("reason"), "stop_evidence_source_mismatch");
    EXPECT_EQ(world.ticks, 1U);
    EXPECT_TRUE(world.port.commands().empty());
    EXPECT_EQ(world.port.ReadGoal(kOriginal), original_goal);
  }
}

TEST(ParticipantStopStageHelperTest, CurrentTerminalEvidenceCannotUseAnAbsentOrExecutingRecord) {
  for (const bool absent : {false, true}) {
    SCOPED_TRACE(absent);
    World world(false);
    GoalReadObservingPort observing(world.port);
    world.Build(&observing);
    const auto stage = Stage(0U, Nominal::kGoalCancel);
    world.Dispatch(stage);
    world.Finish(kOriginal, ExecutionGoalResult::kSucceeded);
    const auto original_goal = world.port.ReadGoal(kOriginal);
    world.clock.advance(500'000'000);
    const auto started = world.clock.now();
    world.on_tick = [&] { world.Sample(2.0, 2.0, 0.0); };
    observing.on_read_goal = [&](auto& goal) {
      if (world.ticks == 0U) return;
      if (absent) {
        goal.reset();
      } else {
        goal->phase = ExecutionGoalPhase::kExecuting;
        goal->result.reset();
        goal->terminal_local_receipt_stamp.reset();
      }
    };
    const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
    ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kFailed);
    ASSERT_TRUE(result.error.has_value());
    EXPECT_EQ(result.error->details.at("reason"), "stop_goal_terminal_state_mismatch");
    EXPECT_EQ(world.clock.now() - started, world.config.hard_stop_completion_timeout_ns);
    EXPECT_TRUE(world.port.commands().empty());
    EXPECT_EQ(world.port.ReadGoal(kOriginal), original_goal);
  }
}

TEST(ParticipantStopStageHelperTest, NominalGoalCancelRequiresTheCancelFinishAndTheGoalTerminal) {
  World world;
  const auto stage = Stage(0U, Nominal::kGoalCancel);
  world.Dispatch(stage);
  world.Sample(0.5, 0.5, 1.0);
  world.on_tick = [&] {
    if (Cancels(world.port).empty()) return;
    world.port.MarkCancelSent(kOriginal, world.clock.now() - 1);
    world.port.AcknowledgeCancel(kOriginal, world.clock.now());
    world.Sample(0.001, 0.52, 0.0);
    if (world.ticks == 6U) world.Finish(kOriginal, ExecutionGoalResult::kCanceled);
  };
  const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
  ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
      << result.error->details.at("reason");
  EXPECT_TRUE(Replaces(world.port).empty());  // a goal cancel is level 4 itself
  EXPECT_GE(world.ticks, 6U);
  EXPECT_TRUE(result.controller_acknowledgement_sequence.has_value());
  EXPECT_TRUE(EvidenceObject(result).contains("goal_observation"));
}

// --- Stop at an applied speed factor of 0 -----------------------------------------

TEST(ParticipantStopStageHelperTest, AppliedFactorZeroAtTheStopRequestSelectsThePlainCancel) {
  // Only an applied 0 (from the sample associated with the goal) makes
  // the Stop the plain cancel; the stage is tagged for the Kernel before any
  // command, whatever was last sent.
  for (const double frozen : {0.0, 0.5}) {
    SCOPED_TRACE(frozen);
    World world;
    world.frozen = frozen;
    const auto stage = Stage(0U);
    world.Dispatch(stage);
    world.Sample(0.5, 0.5, 0.0, 0.0);
    const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
    EXPECT_EQ(result.attempt_disposition, StopStageAttemptDispositionDTO::kNotDispatched);
    EXPECT_EQ(result.error->details.at("reason"), "controlled_stop_applied_factor_zero");
    EXPECT_EQ(result.error->details.at("stop_disposition"), "applied_factor_zero_hard_stop");
    EXPECT_TRUE(world.port.commands().empty());
  }
  // A sample not associated with the goal (received before its feedback,
  // older by source stamp) never decides it.
  World world;
  const auto stage = Stage(0U);
  world.Sample(0.5, 0.5, 0.0, 0.0);
  world.clock.advance(5);
  world.Dispatch(stage);
  const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
  EXPECT_FALSE(result.error->details.contains("stop_disposition"));
}

// --- Controller evidence association (counterexamples) ------------------------

ExecutionGoalObservationDTO GoalWithFeedback(TimePoint source, TimePoint local, double cursor) {
  ExecutionGoalObservationDTO goal;
  goal.key = kOriginal;
  goal.binding_epoch = ExecutionBindingEpoch(1U);
  goal.phase = ExecutionGoalPhase::kExecuting;
  ExecutionGoalFeedbackDTO feedback;
  feedback.source_stamp_ns = source;
  feedback.local_receipt_stamp_ns = local;
  feedback.cursor_s = cursor;
  goal.latest_feedback = feedback;
  return goal;
}

ExecutionControllerSampleDTO StateSample(TimePoint source, TimePoint local, double cursor,
                                         std::uint64_t epoch = 1U) {
  ExecutionControllerSampleDTO sample;
  sample.source_stamp_ns = source;
  sample.local_receipt_stamp_ns = local;
  sample.binding_epoch = ExecutionBindingEpoch(epoch);
  sample.cursor_s = cursor;
  return sample;
}

TEST(ExecutionSampleAssociationTest, QueuedPredecessorStateOlderThanTheOwnFeedbackIsRejected) {
  // (a) own feedback source 200 / local 30 / cursor 0.01, then a queued
  // predecessor state source 190 / local 31 / cursor 4.
  ExecutionSampleAssociation association(ExecutionBindingEpoch(1U), StateSample(150, 20, 3.9), 25);
  const auto goal = GoalWithFeedback(200, 30, 0.01);
  EXPECT_FALSE(association.Observe(StateSample(190, 31, 4.0), goal));
  EXPECT_TRUE(association.Observe(StateSample(200, 32, 0.01), goal));
  EXPECT_TRUE(association.Observe(StateSample(210, 33, 0.02), goal));
  // Evidence stays ordered by source stamp; another epoch never compares.
  EXPECT_FALSE(association.Observe(StateSample(205, 34, 0.015), goal));
  EXPECT_FALSE(association.Observe(StateSample(220, 35, 0.03, 2U), goal));
  EXPECT_EQ(association.recognition_delay_ns(), 5);
}

TEST(ExecutionSampleAssociationTest, OldTrajectoryHoldBeforeTheNewGoalsFeedbackIsRejected) {
  // (b) after a new dispatch, the old trajectory's natural-completion hold
  // (cursor 0, newer than the pre-dispatch sample) arrives before the new
  // goal's feedback: a cursor restart never anchors.
  ExecutionSampleAssociation association(ExecutionBindingEpoch(1U), StateSample(100, 10, 2.0), 12);
  ExecutionGoalObservationDTO pending;
  pending.key = kOriginal;
  pending.binding_epoch = ExecutionBindingEpoch(1U);
  pending.phase = ExecutionGoalPhase::kAccepted;
  EXPECT_FALSE(association.Observe(StateSample(110, 13, 0.0), pending));
  EXPECT_FALSE(association.anchored());
  EXPECT_FALSE(association.recognition_delay_ns().has_value());
  const auto executing = GoalWithFeedback(120, 15, 0.0);
  EXPECT_FALSE(association.Observe(StateSample(110, 16, 0.0), executing));
  EXPECT_TRUE(association.Observe(StateSample(121, 17, 0.01), executing));
}

TEST(ExecutionSampleAssociationTest, ResultBeforeFeedbackAnchorsWithoutAssigningSamples) {
  ExecutionSampleAssociation association(ExecutionBindingEpoch(1U), StateSample(100, 10, 2.0), 12);
  ExecutionGoalObservationDTO ended;
  ended.key = kOriginal;
  ended.binding_epoch = ExecutionBindingEpoch(1U);
  ended.phase = ExecutionGoalPhase::kTerminal;
  ended.result = ExecutionGoalResult::kSucceeded;
  ended.terminal_local_receipt_stamp = 20;
  EXPECT_TRUE(association.ObserveGoal(ended));
  EXPECT_EQ(association.recognition_delay_ns(), 8);
  // No source stamp is invented for the result; no sample is associated.
  EXPECT_FALSE(association.Observe(StateSample(130, 21, 0.0), ended));
  // The completion gate takes a sample newer than the pre-dispatch sample.
  EXPECT_FALSE(association.NewerThanEvidence(StateSample(100, 22, 0.0)));
  EXPECT_TRUE(association.NewerThanEvidence(StateSample(101, 23, 0.0)));
}

/// Scripts a level-3 brake whose evidence is ordered by source stamp: the
/// brake is accepted with feedback; braking samples; its kSucceeded; then a
/// delayed old zero sample at the brake's end with an older source stamp;
/// only from `genuine_tick` on JTC's genuine success hold.
TEST(ParticipantStopStageHelperTest, DelayedOldZeroSampleAfterTheBrakeResultIsRejected) {
  // (c)
  World world;
  const auto stage = Stage(1U);
  world.Dispatch(stage);
  world.Sample(0.5, 0.5, 1.0);
  std::size_t started = 0U;
  std::optional<TimePoint> braking_source;
  constexpr std::size_t kGenuineTick = 8U;
  world.on_tick = [&] {
    if (Replaces(world.port).empty()) return;
    if (started == 0U) {
      started = world.ticks;
      world.Executing(kSecond);
      world.port.FinishGoal(kOriginal, ExecutionGoalResult::kPreemptedBy, kSecond,
                            world.clock.now());
    }
    const auto tick = world.ticks - started + 1U;
    if (tick <= 3U) {
      world.Sample(0.004 * static_cast<double>(tick), 0.55, 0.2);  // braking
      braking_source = world.clock.now();
    } else if (tick == 4U) {
      world.Finish(kSecond, ExecutionGoalResult::kSucceeded);
    } else if (tick < kGenuineTick) {
      // Received after the result, at zero velocity on the brake's end, but
      // older by source stamp than the brake's own evidence.
      world.Sample(0.0, world.BrakeEnd(), 0.0, 0.5, std::nullopt, *braking_source - 1);
    } else {
      world.Sample(0.0, world.BrakeEnd(), 0.0);  // JTC's genuine success hold
    }
  };
  const auto result = world.helper->TranslateStopStage(stage, Budget(stage, 1'800'000'000), {});
  ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
      << result.error->details.at("reason");
  EXPECT_GE(world.ticks, started + kGenuineTick - 1U);
  EXPECT_EQ(Text(EvidenceObject(result), "hard_stop_level"), "controlled_hard_stop");
  EXPECT_TRUE(EvidenceObject(result).contains("goal_recognition_delay_ns"));

  // A zero sample newer than the evidence but away from the brake's end is
  // not the success hold either: the hard-stop bound decides.
  World away;
  const auto again = Stage(1U);
  away.Dispatch(again);
  away.Sample(0.5, 0.5, 1.0);
  away.on_tick = [&] {
    if (Replaces(away.port).empty()) return;
    away.Executing(kSecond);
    away.Finish(kSecond, ExecutionGoalResult::kSucceeded);
    away.Sample(0.0, away.BrakeEnd() + 1e-6, 0.0);
  };
  const auto failed = away.helper->TranslateStopStage(again, Budget(again, 1'800'000'000), {});
  EXPECT_EQ(failed.error->details.at("reason"), "stop_evidence_phase_expired");
}

TEST(ParticipantStopStageHelperTest, BrakeFeedbackReadInTheFinishPollCountsBeforeTheHold) {
  // Retained evidence source 180; in one poll the
  // brake's own feedback (source 200), its kSucceeded and a zero-velocity
  // sample on the brake's end (source 190) are read together. The feedback is
  // ingested first, so 190 is not newer than the newest evidence and does not
  // finish; only a later genuine hold newer than 200 does.
  World world;
  const auto stage = Stage(1U);
  world.Dispatch(stage);
  world.Sample(0.5, 0.5, 1.0);
  std::size_t started = 0U;
  std::optional<TimePoint> evidence_source;  // "180"
  std::size_t counterexample_tick = 0U;
  constexpr std::size_t kGenuineTick = 8U;
  world.on_tick = [&] {
    if (Replaces(world.port).empty()) return;
    if (started == 0U) {
      started = world.ticks;
      world.Executing(kSecond);
      world.port.FinishGoal(kOriginal, ExecutionGoalResult::kPreemptedBy, kSecond,
                            world.clock.now());
    }
    const auto tick = world.ticks - started + 1U;
    if (tick <= 3U) {
      world.Sample(0.004 * static_cast<double>(tick), 0.55, 0.2);  // braking
      evidence_source = world.clock.now();
    } else if (tick == 4U) {
      ExecutionGoalFeedbackDTO feedback;
      feedback.source_stamp_ns = *evidence_source + 20;  // "200"
      feedback.local_receipt_stamp_ns = world.clock.now();
      world.port.SetGoalFeedback(kSecond, feedback);
      world.Finish(kSecond, ExecutionGoalResult::kSucceeded);
      world.Sample(0.0, world.BrakeEnd(), 0.0, 0.5, std::nullopt,
                   *evidence_source + 10);  // "190"
      counterexample_tick = world.ticks;
    } else if (tick >= kGenuineTick) {
      world.Sample(0.0, world.BrakeEnd(), 0.0);  // genuine hold, newer than 200
    }
  };
  const auto result = world.helper->TranslateStopStage(stage, Budget(stage, 1'800'000'000), {});
  ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
      << result.error->details.at("reason");
  ASSERT_GT(counterexample_tick, 0U);
  EXPECT_GE(world.ticks, started + kGenuineTick - 1U);
  const auto& evidence = EvidenceObject(result);
  EXPECT_EQ(Text(evidence, "hard_stop_level"), "controlled_hard_stop");
  EXPECT_EQ(Text(std::get<JsonObjectDTO>(evidence.at("post_command_sample").value),
                 "finish_basis"),
            "controlled_hard_stop_succeeded_then_hold");
}

TEST(ParticipantStopStageHelperTest, ShortBrakeFinishedBeforeAnyFeedbackFinishesOnAGenuineHold) {
  // (d) the brake's result arrives before any feedback: the result anchors it
  // (installed), and the first sample newer than the newest sample seen
  // before the Replace, at zero velocity on the brake's end, finishes it.
  World world;
  const auto stage = Stage(1U);
  world.Dispatch(stage);
  world.Sample(0.5, 0.5, 1.0);
  std::size_t step = 0U;
  world.on_tick = [&] {
    if (Replaces(world.port).empty()) return;
    ++step;
    if (step == 1U) {
      world.port.FinishGoal(kOriginal, ExecutionGoalResult::kPreemptedBy, kSecond,
                            world.clock.now());
      world.Finish(kSecond, ExecutionGoalResult::kSucceeded);  // no feedback at all
    } else {
      world.Sample(0.0, world.BrakeEnd(), 0.0);  // hold at cursor 0
    }
  };
  const auto result = world.helper->TranslateStopStage(stage, Budget(stage, 1'800'000'000), {});
  ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
      << result.error->details.at("reason");
  const auto& evidence = EvidenceObject(result);
  EXPECT_EQ(Text(evidence, "hard_stop_level"), "controlled_hard_stop");
  EXPECT_EQ(Text(std::get<JsonObjectDTO>(evidence.at("post_command_sample").value),
                 "finish_basis"),
            "controlled_hard_stop_succeeded_then_hold");
  EXPECT_TRUE(Cancels(world.port).empty());
}

TEST(ParticipantStopStageHelperTest, CancelledGoalThatSucceededTakesTheNaturalCompletionPath) {
  // (e) the cancelled goal's own result is SUCCEEDED: never counted as cancel
  // braking, even with an acknowledged cancel and a restart at zero velocity.
  World world(false);
  world.config.max_deceleration_on_cancel_rad_s2.clear();  // level 4 directly
  world.Build();
  const auto stage = Stage(1U);
  world.Dispatch(stage);
  world.Sample(1.9, 1.9, 1.0);
  world.on_tick = [&] {
    if (Cancels(world.port).empty()) return;
    world.port.MarkCancelSent(kOriginal, world.clock.now() - 1);
    world.port.AcknowledgeCancel(kOriginal, world.clock.now());
    world.Finish(kOriginal, ExecutionGoalResult::kSucceeded);
    world.clock.advance(1);
    world.Sample(0.0, 2.0, 0.0);  // JTC's success hold
  };
  const auto result = world.helper->TranslateStopStage(stage, Budget(stage, 1'800'000'000), {});
  ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
      << result.error->details.at("reason");
  const auto& evidence = EvidenceObject(result);
  EXPECT_EQ(Text(evidence, "hard_stop_level"), "goal_already_terminal");
  EXPECT_EQ(Text(std::get<JsonObjectDTO>(evidence.at("post_command_sample").value),
                 "finish_basis"),
            "goal_already_terminal");
}

TEST(ParticipantStopStageHelperTest, EndedGoalHoldNeedsANewerSourceStampThanTheGoalsEvidence) {
  // No new rule: the goal already ended; a zero-reference
  // sample with an OLDER controller source stamp, queued until after the
  // terminal result, is not the hold. Only a genuinely newer zero sample is.
  World world;
  const auto stage = Stage(1U);
  world.Dispatch(stage);  // own feedback anchors the goal at now - 1
  const TimePoint anchor_stamp = world.clock.now() - 1;
  world.clock.advance(kTick);
  world.Sample(1.0, 1.0, 1.0, 0.5, std::nullopt, world.clock.now());
  world.Finish(kOriginal, ExecutionGoalResult::kSucceeded);
  std::size_t genuine_tick = 0U;
  world.on_tick = [&] {
    if (world.ticks == 1U) {
      // Older zero sample (source stamp before the goal's own feedback),
      // received only now, after the terminal result.
      world.Sample(0.0, 1.0, 0.0, 0.5, std::nullopt, anchor_stamp - 5);
    } else if (world.ticks == 5U) {
      genuine_tick = world.ticks;
      world.Sample(0.0, 2.0, 0.0);  // the genuine newer hold
    }
  };
  const auto result = world.helper->TranslateStopStage(stage, Budget(stage, 1'800'000'000), {});
  ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
      << result.error->details.at("reason");
  EXPECT_EQ(Text(EvidenceObject(result), "hard_stop_level"), "goal_already_terminal");
  ASSERT_EQ(genuine_tick, 5U);
  EXPECT_EQ(world.ticks, genuine_tick);  // not finished on the older zero sample
}

TEST(ParticipantStopStageHelperTest, CancelBaselineMustBeAttributedToTheCancelledGoal) {
  // The Stop precedes the goal's first own-associated sample. The
  // newest sample at the cancel is a pre-goal hold with a high cursor; the
  // original goal's start sample (cursor 0, reference at rest, newer source
  // stamp) arrives after the cancel with the ACK and CANCELED already known.
  // It must not pass as the switch plus rest: without a baseline the
  // association attributes to the cancelled goal, no switch can be
  // established and the stage fails closed at its deadline.
  World world;
  const auto stage = Stage(0U, Nominal::kGoalCancel);
  world.Sample(0.30, 0.5, 0.0);  // the controller's pre-goal hold
  world.helper->BindDispatchedGoal(Goal(stage), std::make_shared<ExecutionSampleAssociation>(
                                                    ExecutionBindingEpoch(1U), world.port.ReadSample(),
                                                    world.clock.now()));
  ExecutionGoalObservationDTO goal;  // accepted, no feedback yet
  goal.key = kOriginal;
  goal.binding_epoch = ExecutionBindingEpoch(1U);
  goal.phase = ExecutionGoalPhase::kExecuting;
  goal.mechanism_goal_id = MechanismGoalId("uuid-1");
  world.port.SetGoal(goal);
  bool sent = false;
  world.on_tick = [&] {
    if (Cancels(world.port).empty()) return;
    if (!sent) {
      sent = true;
      world.port.MarkCancelSent(kOriginal, world.clock.now() - 1);
      world.Cancelled(kOriginal);  // ACK and CANCELED known first
      world.Sample(0.0, 0.5, 0.0);  // then the original goal's start sample
    }
  };
  const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
  EXPECT_EQ(Cancels(world.port), std::vector<ExecutionGoalKey>{kOriginal});
  ASSERT_NE(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
      << "false finish on the original goal's start sample";
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->details.at("reason"), "stop_evidence_phase_expired");
}

TEST(ParticipantStopStageHelperTest,
     LateInstalledBrakeAfterTheCancelFinishesOnlyOnAZeroReferenceAfterTheSwitch) {
  // Residual case, no new rule: under the attributed
  // baseline and the lead-in window, level 3 shows no own feedback within
  // 100 ms, so the cancel follows (level 4). The controller then installs the
  // brake late: its samples (cursor restarted below the attributed baseline,
  // reference still moving) arrive after the cancel was sent, with the ACK
  // and CANCELED already known. The cancel predicate may take the brake's
  // cursor restart as the switch (the residual ambiguity), but it finishes
  // only on a later sample whose reference velocity is exactly zero; while
  // the commanded reference still moves, the stage does not finish.
  World world(false);
  world.config.stop_traj_delta_s = 0.1;
  world.Build();
  const auto stage = Stage(1U);
  world.Dispatch(stage);
  world.Sample(0.5, 0.5, 1.0);  // the original goal's sample: the attributed baseline
  std::size_t cancel_tick = 0U;
  std::size_t moving_ticks_after_cancel = 0U;
  std::size_t zero_tick = 0U;
  std::optional<TimePoint> zero_source;
  world.on_tick = [&] {
    if (Replaces(world.port).empty()) return;
    if (Cancels(world.port).empty()) {
      world.port.SetGoalPhase(kSecond, ExecutionGoalPhase::kExecuting);  // accepted only
      return;
    }
    if (cancel_tick == 0U) {
      cancel_tick = world.ticks;
      ExecutionGoalFeedbackDTO feedback;  // the brake's own feedback, late
      feedback.source_stamp_ns = world.clock.now() - 1;
      feedback.local_receipt_stamp_ns = world.clock.now() - 1;
      world.port.SetGoalFeedback(kSecond, feedback);
      world.port.MarkCancelSent(kSecond, world.clock.now() - 1);
      world.port.MarkCancelSent(kOriginal, world.clock.now() - 1);
      world.Cancelled(kSecond);  // ACK and CANCELED known
      world.Cancelled(kOriginal);
    }
    const auto since = world.ticks - cancel_tick;
    if (since < 6U) {
      // The late brake: cursor restarted (below the baseline 0.5), reference
      // still moving.
      world.Sample(0.004 * static_cast<double>(since + 1U), 0.6, 0.04);
      ++moving_ticks_after_cancel;
    } else {
      if (zero_tick == 0U) zero_tick = world.ticks;
      world.Sample(0.03, 0.61, 0.0);  // the reference at rest
      if (!zero_source) zero_source = world.port.ReadSample()->source_stamp_ns;
    }
  };
  const auto result = world.helper->TranslateStopStage(stage, Budget(stage, 1'800'000'000), {});
  ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
      << result.error->details.at("reason");
  const auto& evidence = EvidenceObject(result);
  EXPECT_EQ(Text(evidence, "hard_stop_fallback_reason"), "controlled_hard_stop_not_installed");
  EXPECT_EQ(Text(evidence, "hard_stop_level"), "jtc_cancel_decelerate_on_cancel");
  EXPECT_EQ(Text(std::get<JsonObjectDTO>(evidence.at("post_command_sample").value),
                 "finish_basis"),
            "cancel_reference_at_rest");
  // Six moving samples of the late brake passed (ACK + CANCELED known) without
  // a finish; the finish is the first exactly-zero reference sample.
  EXPECT_EQ(moving_ticks_after_cancel, 6U);
  ASSERT_NE(zero_tick, 0U);
  // The finish is the first zero-reference sample: no later tick was needed.
  EXPECT_EQ(world.ticks, zero_tick);
  ASSERT_TRUE(zero_source.has_value());
}

// --- Proposed fallback rule evidence: not product behaviour --------------------
//
// The proposed narrow fallback rule is NOT implemented in the
// helper. `ProposedFallbackCancelFinish` below restates it in test code only,
// so its counterexamples can be checked: when level 3 falls back to the
// cancel, the cancel's switch baseline is the newest sample received before
// the level-3 brake was sent; everything else is today's RunCancel predicate
// (receipt after the cancel was sent, source stamp newer than the baseline,
// switch = cursor below the baseline cursor, finish = positive ACK + switch +
// zero reference velocity + CANCELED result). Each check states what the rule
// does with the counterexample; the required outcome is "not a finish".

struct ProposedFinish {
  bool finished{false};
  /// The sample the rule takes as the cancel's switch, and the finish sample.
  std::optional<TimePoint> switch_source;
  std::optional<TimePoint> finish_source;
};

/// Evaluates the proposed rule poll by poll over samples in receipt order
/// (each poll sees the newest sample). `ack_at`/`result_at`: local receipts
/// of the positive ACK and of the CANCELED result.
ProposedFinish ProposedFallbackCancelFinish(const ExecutionControllerSampleDTO& pre_brake,
                                            TimePoint cancel_sent, TimePoint ack_at,
                                            TimePoint result_at,
                                            const std::vector<ExecutionControllerSampleDTO>& arrivals) {
  ProposedFinish out;
  for (const auto& sample : arrivals) {
    const auto now = sample.local_receipt_stamp_ns;
    if (now <= cancel_sent || sample.binding_epoch != pre_brake.binding_epoch ||
        sample.source_stamp_ns <= pre_brake.source_stamp_ns) {
      continue;
    }
    if (!out.switch_source && sample.cursor_s < pre_brake.cursor_s) {
      out.switch_source = sample.source_stamp_ns;
    }
    const bool acknowledged = now >= ack_at;
    const bool canceled = now >= result_at;
    const bool at_rest = std::all_of(sample.reference_velocities_rad_s.begin(),
                                     sample.reference_velocities_rad_s.end(),
                                     [](double v) { return v == 0.0; });
    if (acknowledged && canceled && out.switch_source &&
        sample.source_stamp_ns >= *out.switch_source && at_rest) {
      out.finished = true;
      out.finish_source = sample.source_stamp_ns;
      return out;
    }
  }
  return out;
}

ExecutionControllerSampleDTO Arrival(TimePoint source_ms, TimePoint receipt_ms, double cursor,
                                     double velocity) {
  ExecutionControllerSampleDTO sample;
  sample.source_stamp_ns = source_ms * 1'000'000;
  sample.local_receipt_stamp_ns = receipt_ms * 1'000'000;
  sample.binding_epoch = ExecutionBindingEpoch(1U);
  sample.cursor_s = cursor;
  sample.reference_positions_rad = {0.5};
  sample.reference_velocities_rad_s = {velocity};
  sample.measured_positions_rad = {0.5};
  return sample;
}

constexpr TimePoint kMs = 1'000'000;

TEST(ProposedNarrowFallbackRuleEvidence, DelayedLevelThreeBrakeSamplesAreTakenAsTheCancelSwitch) {
  // Original goal at cursor 0.40 (pre-brake baseline, source 100 ms). The
  // level-3 Replace is sent at 102 ms, not installed within the 20 ms
  // allowance; the cancel is sent at 125 ms (ACK 130, CANCELED 131).
  const auto pre_brake = Arrival(100, 101, 0.40, 0.5);
  // Delayed samples of the level-3 brake (its own cursor restart), produced
  // before the cancel took effect, received after the cancel was sent.
  const auto rule = ProposedFallbackCancelFinish(
      pre_brake, 125 * kMs, 130 * kMs, 131 * kMs,
      {Arrival(110, 126, 0.01, 0.40), Arrival(120, 127, 0.02, 0.30)});
  EXPECT_FALSE(rule.finished);  // moving samples: no finish by themselves
  // ...but the rule takes the level-3 Replace restart as the cancel switch:
  // the pre-brake cursor alone cannot tell the two restarts apart.
  ASSERT_TRUE(rule.switch_source.has_value());
  EXPECT_EQ(*rule.switch_source, 110 * kMs) << "switch attributed to a level-3 brake sample";
}

TEST(ProposedNarrowFallbackRuleEvidence, TheBrakesEndpointZeroSampleCompletesTheCancel) {
  // As above; the brake's endpoint (its JTC success hold, reference velocity
  // exactly zero) is stamped 124 ms, before the cancel could take effect,
  // and received at 132 ms, after the ACK and the CANCELED result.
  const auto pre_brake = Arrival(100, 101, 0.40, 0.5);
  const auto rule = ProposedFallbackCancelFinish(
      pre_brake, 125 * kMs, 130 * kMs, 131 * kMs,
      {Arrival(110, 126, 0.01, 0.40), Arrival(124, 132, 0.05, 0.0)});
  // Required: not a finish. Observed: the proposed rule finishes on it.
  EXPECT_TRUE(rule.finished) << "the counterexample is expected to be accepted by the rule";
  EXPECT_EQ(rule.finish_source, 124 * kMs);
}

TEST(ProposedNarrowFallbackRuleEvidence, OriginalGoalsStartSampleCompletesTheCancelInTheFirstCycle) {
  // First-cycle case: the newest sample before the brake predates the
  // original goal (the previous hold, cursor 0.30, source 100 ms). The
  // original goal's own start sample (cursor 0, reference velocity 0,
  // source 105 ms) arrives at 132 ms, after the ACK and the CANCELED result.
  const auto pre_brake = Arrival(100, 101, 0.30, 0.0);
  const auto rule = ProposedFallbackCancelFinish(pre_brake, 125 * kMs, 130 * kMs, 131 * kMs,
                                                 {Arrival(105, 132, 0.0, 0.0)});
  // Required: not a finish. Observed: the proposed rule finishes on it.
  EXPECT_TRUE(rule.finished) << "the counterexample is expected to be accepted by the rule";
  EXPECT_EQ(rule.finish_source, 105 * kMs);

  // With a pre-brake baseline taken on the original goal itself (cursor
  // 0.40 at 100 ms) the same start sample is older and is rejected.
  const auto mid_motion = ProposedFallbackCancelFinish(
      Arrival(100, 101, 0.40, 0.5), 125 * kMs, 130 * kMs, 131 * kMs, {Arrival(95, 132, 0.0, 0.0)});
  EXPECT_FALSE(mid_motion.finished);
}

TEST(ProposedNarrowFallbackRuleEvidence, AckAndCanceledAloneNeverCompleteTheCancel) {
  // ACK and the CANCELED result arrive while every sample still moves: no
  // finish (the ACK/result do not prove that braking finished).
  const auto rule = ProposedFallbackCancelFinish(
      Arrival(100, 101, 0.40, 0.5), 125 * kMs, 126 * kMs, 126 * kMs,
      {Arrival(130, 131, 0.01, 0.3), Arrival(140, 141, 0.02, 0.2)});
  EXPECT_FALSE(rule.finished);
}

// --- First-cycle Stop: containment now, completion proof at the deadline -------

TEST(ParticipantStopStageHelperTest, FirstCycleStopCancelsAtOnceWhileOnlyTheCompletionProofWaits) {
  // A Stop inside the original goal's first controller period: the goal is
  // executing (no feedback yet) and no controller sample exists. Level 3
  // cannot plan without a sample, so the stage falls to the cancel; the
  // cancel is dispatched before the first wait. The controller then brakes
  // genuinely (cancel trajectory to zero, ACK, CANCELED), but no sample can
  // show the switch against a missing baseline: only the completion proof
  // waits, until the Stop deadline (stop_evidence_phase_expired).
  World world;
  const auto stage = Stage(1U);
  world.helper->BindDispatchedGoal(Goal(stage), std::make_shared<ExecutionSampleAssociation>(
                                                    ExecutionBindingEpoch(1U), std::nullopt,
                                                    world.clock.now()));
  ExecutionGoalObservationDTO goal;
  goal.key = kOriginal;
  goal.binding_epoch = ExecutionBindingEpoch(1U);
  goal.phase = ExecutionGoalPhase::kExecuting;
  goal.mechanism_goal_id = MechanismGoalId("uuid-1");
  world.port.SetGoal(goal);
  const auto started = world.clock.now();
  std::optional<std::size_t> cancel_seen_at_tick;
  std::optional<TimePoint> cancel_seen_at;
  world.on_tick = [&] {
    if (Cancels(world.port).empty()) return;
    if (!cancel_seen_at_tick) {
      cancel_seen_at_tick = world.ticks;
      cancel_seen_at = world.clock.now();
      world.port.MarkCancelSent(kOriginal, started);
    }
    // The genuine cancel braking: its own cursor from 0, velocity to zero.
    const auto step = world.ticks - *cancel_seen_at_tick + 1U;
    world.Sample(0.01 * static_cast<double>(step), 0.5, step >= 4U ? 0.0 : 0.1);
    if (step == 2U) world.Cancelled(kOriginal);
  };
  const auto result = world.helper->TranslateStopStage(stage, Budget(stage, 1'800'000'000), {});
  // Containment: the cancel was already sent when the first wait returned.
  ASSERT_TRUE(cancel_seen_at_tick.has_value());
  EXPECT_EQ(*cancel_seen_at_tick, 1U);
  EXPECT_EQ(Cancels(world.port), std::vector<ExecutionGoalKey>{kOriginal});
  EXPECT_TRUE(Replaces(world.port).empty());
  EXPECT_LE(*cancel_seen_at - started, 10'000'000);  // within the first tick
  // Completion evidence: never proven; the stage fails only at the deadline.
  EXPECT_EQ(result.attempt_disposition, StopStageAttemptDispositionDTO::kDispatched);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->details.at("reason"), "stop_evidence_phase_expired");
  EXPECT_EQ(result.error->details.at("hard_stop_level"), "jtc_cancel_decelerate_on_cancel");
  // The wait runs to the stage's terminal deadline (here the budget's 0.9 s
  // evidence phase, inside the 1 s hard-stop bound), long after the brake
  // genuinely ended (4 samples, about 40 ms after the cancel).
  EXPECT_GE(world.clock.now() - started, 900'000'000);
  EXPECT_LE(world.clock.now() - started, world.config.hard_stop_completion_timeout_ns);
}


// Full precision actual Core payloads for the separate pristine-A receiver gate.
std::string ReceiverCurve(const TrajectoryDTO& trajectory) {
  std::ostringstream out;
  out << std::setprecision(17);
  for (const auto& point : trajectory.waypoints) {
    out << point.time_from_start_s;
    for (const auto* values : {&point.positions_rad, &point.velocities_rad_s,
                               &point.accelerations_rad_s2})
      for (const double value : *values) out << ',' << value;
    out << ';';
  }
  return out.str();
}

// ACTUAL-C handover, against the commands actually issued: the controller
// commands path C + k f P at tick k. Ticks k < 0 were the unmodified source
// (Original(): x = path time, v = 1, a = 0); k >= 0 come from the sent payload
// (its time 0 is E = C - r). Returns the wall finite differences (rad/s^2,
// rad/s^3) at k = 0 and k = 1, which are NOT the analytic law jerk.
struct Handover {
  double fd_a[2];
  double fd_j[2];
  double analytic_a[2];
};
Handover ExpectActualC(const TrajectoryDTO& sent, double c, double r, double factor,
                       double period) {
  const auto& first = sent.waypoints.front();
  EXPECT_NEAR(first.positions_rad[0], c, 2e-8);  // the receiver's source(C) check, unchanged
  EXPECT_NEAR(first.positions_rad[0], c, 1e-10);
  EXPECT_NEAR(first.velocities_rad_s[0], 1.0, 1e-10);
  EXPECT_NEAR(first.accelerations_rad_s2[0], 0.0, 1e-8);
  std::vector<double> q;
  std::vector<double> a;
  for (int k = -3; k <= 3; ++k) {
    const double step = factor * period * static_cast<double>(k);
    if (k < 0) {
      q.push_back(c + step);
      a.push_back(0.0);
      continue;
    }
    const auto state = sample_trajectory(sent, r + step);
    EXPECT_TRUE(state.ok());
    if (!state.ok()) return {};
    q.push_back(state.value().positions_rad[0]);
    a.push_back(state.value().accelerations_rad_s2[0] * factor * factor);  // wall
  }
  Handover out{};
  for (int i = 0; i < 2; ++i) {
    const auto k = static_cast<std::size_t>(3 + i);
    out.fd_a[i] = (q[k + 1] - 2.0 * q[k] + q[k - 1]) / (period * period);
    out.fd_j[i] = (q[k + 1] - 3.0 * q[k] + 3.0 * q[k - 1] - q[k - 2]) / (period * period * period);
    out.analytic_a[i] = a[k];
  }
  return out;
}

// Wall acceleration at every sent knot (analytic, from the planner's knots).
double PeakKnotDeceleration(const TrajectoryDTO& sent, double factor) {
  double peak = 0.0;
  for (const auto& point : sent.waypoints)
    peak = std::max(peak, std::abs(point.accelerations_rad_s2[0]) * factor * factor);
  return peak;
}

TEST(ParticipantStopStageHelperTest, ProductionTimingMatrixExportsActualCorePayloads) {
  for (const double rate : {100.0, 150.0}) for (const double factor : {1.0, 0.5, 0.1})
    for (const char* mode : {"soft_on_path", "spring_back_hold", "spring_back_fast", "hard"}) {
      SCOPED_TRACE(::testing::Message() << rate << ' ' << factor << ' ' << mode);
      World world(false);
      world.config.stop_traj_delta_s = 0.1;  // retained production value, never enlarged
      auto lane = world.port.ReadLaneFacts();
      lane.supports_scheduled_replacement = true;
      lane.controller_update_rate_hz = rate;
      lane.experiment.midtick_stamp = true;
      world.port.SetLaneFacts(lane);
      world.frozen = factor;
      world.Build();
      auto stage = Stage(std::string(mode) == "hard" ? 1U : 0U, Nominal::kControlled,
                         Parameters(std::string(mode) == "hard" ? "soft_on_path" : mode,
                                    false, std::nullopt, 10.0));
      world.Dispatch(stage);
      world.Sample(0.5, 0.5, 1.0, factor);
      const auto source = *world.port.ReadSample();
      ScriptBrake(world, kSecond, 4U, factor);
      auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
      // The C-lead bound includes one unscaled P: UR/.1 soft/hold no longer fall through.
      const bool falls_through = result.outcome == ParticipantStopStageOutcomeDTO::kFailed;
      ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
          << (result.error ? result.error->message : "");
      const auto calls = Replaces(world.port);
      ASSERT_EQ(calls.size(), 1U);
      const auto& call = calls.front();
      ASSERT_TRUE(call.start);
      const auto& timing = *call.start->experiment;
      const double period = static_cast<Duration>(1e9 / rate) / 1e9;
      const double b = factor * static_cast<double>(static_cast<Duration>(1e9 / rate) / 2) / 1e9;
      const double c = source.cursor_s + factor *
          static_cast<double>(timing.target_tick_ns - source.source_stamp_ns) / 1e9 + period;
      EXPECT_NEAR(call.trajectory.waypoints.front().time_from_start_s, period - b, 1e-12);
      // ACTUAL-C: a C past the trip point re-plans from source(C), so every row
      // starts at source(C) again.
      EXPECT_NEAR(call.trajectory.waypoints.front().positions_rad[0], c, 1e-10);
      EXPECT_NEAR(call.trajectory.waypoints.front().velocities_rad_s[0], 1.0, 1e-10);
      EXPECT_TRUE(Cancels(world.port).empty());
      const auto label = std::string(mode) + '_' + std::to_string(static_cast<int>(rate)) +
                         '_' + std::to_string(factor);
      std::ostringstream metadata;
      metadata << std::setprecision(17) << rate << ',' << factor << ','
               << call.start->controller_start_stamp_ns << ',' << timing.target_tick_ns << ','
               << source.source_stamp_ns << ',' << source.cursor_s << ',' << c << ','
               << period - b << ',' << falls_through;
      RecordProperty(label + "_metadata", metadata.str());
      RecordProperty(label + "_joints", "joint_1");
      RecordProperty(label + "_source", ReceiverCurve(Original()));
      RecordProperty(label + "_sent", ReceiverCurve(call.trajectory));
    }
}

// LEAD-B: the soft/hold C-lead bound is the f-scaled lead plus one unscaled P.
// A lead ending inside C's own P lookahead is eligible; a C past it is not.
TEST(ParticipantStopStageHelperTest, SoftAndHoldCLeadBoundAddsOneUnscaledPeriod) {
  for (const double rate : {100.0, 150.0}) for (const double factor : {1.0, 0.5, 0.1})
    for (const char* mode : {"soft_on_path", "spring_back_hold"}) for (const bool late : {false, true}) {
      SCOPED_TRACE(::testing::Message() << rate << ' ' << factor << ' ' << mode << " late=" << late);
      const auto period_ns = static_cast<Duration>(1e9 / rate);
      const double period = static_cast<double>(period_ns) / 1e9;
      const auto ticks = (20'000'000 + period_ns - 1) / period_ns;  // existing A20 grid
      const double advance = static_cast<double>(ticks * period_ns) / 1e9;  // T - S
      World world(false);
      // Lead end L = w + f*lead: inside (C - P, C) when eligible, before C - P when late.
      world.config.stop_traj_delta_s = late ? 0.9 * advance : advance + 0.9 * period / factor;
      auto lane = world.port.ReadLaneFacts();
      lane.supports_scheduled_replacement = true;
      lane.controller_update_rate_hz = rate;
      lane.experiment.midtick_stamp = true;
      world.port.SetLaneFacts(lane);
      world.frozen = factor;
      world.Build();
      auto stage = Stage(0U, Nominal::kControlled, Parameters(mode, false, std::nullopt, 10.0));
      world.Dispatch(stage);
      world.Sample(0.5, 0.5, 1.0, factor);
      const auto source = *world.port.ReadSample();
      const double lead_end = source.cursor_s + factor * world.config.stop_traj_delta_s;
      if (late) {
        ScriptBrake(world, kSecond, 4U, factor);
        auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
        ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kFailed);
        ASSERT_TRUE(result.error);
        EXPECT_EQ(result.error->details.at("reason"), std::string(mode == std::string("soft_on_path")
            ? "soft_on_path" : "spring_back") + "_stop_delta_budget_exceeded");
        EXPECT_EQ(result.error->details.at("recommended_disposition"), "hard_stop");
        EXPECT_TRUE(Replaces(world.port).empty());
        stage.stage_index = 1U;  // existing Kernel fallback owner selects the hard stage
        result = world.helper->TranslateStopStage(stage, Budget(stage), {});
        ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
            << (result.error ? result.error->message : "");
        const auto brakes = Replaces(world.port);
        ASSERT_EQ(brakes.size(), 1U);
        EXPECT_GE(brakes.front().trajectory.waypoints.front().positions_rad[0],
                  lead_end + period);  // hard brake entry C
        continue;
      }
      ScriptReplacement(world, kSecond, 5U, ExecutionGoalResult::kSucceeded);
      const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
      ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
          << (result.error ? result.error->message : "");
      const auto calls = Replaces(world.port);
      ASSERT_EQ(calls.size(), 1U);
      ASSERT_TRUE(calls.front().start);
      EXPECT_EQ(calls.front().start->experiment->target_tick_ns - source.source_stamp_ns,
                ticks * period_ns);
      const double c = source.cursor_s + factor * advance + period;
      const auto& first = calls.front().trajectory.waypoints.front();
      EXPECT_GT(c, lead_end);  // the f-scaled lead alone would have refused this C
      EXPECT_LT(c, lead_end + period);
      (void)ExpectActualC(calls.front().trajectory, c, first.time_from_start_s, factor, period);
      EXPECT_TRUE(Cancels(world.port).empty());
    }
}

// ACTUAL-C: one scheduled soft/hold Stop on the production grid (midtick stamp,
// A20). `lead` places L = w + f*lead; C = w + f(T-S) + P.
struct ActualCRun {
  std::unique_ptr<World> world;
  ResolvedParticipantStopStageDTO stage;
  ExecutionControllerSampleDTO source;
  std::optional<ParticipantStopStageResultDTO> result;
  double period{};
  double advance{};
  double c{};
  double lead_end{};
};
ActualCRun RunActualC(double rate, double factor, double lead,
                      JsonObjectDTO parameters, std::optional<Duration> remaining = std::nullopt) {
  ActualCRun run{std::make_unique<World>(false),
                 Stage(0U, Nominal::kControlled, std::move(parameters)), {}, {}, {}, {}, {}, {}};
  const auto period_ns = static_cast<Duration>(1e9 / rate);
  run.period = static_cast<double>(period_ns) / 1e9;
  const auto ticks = (20'000'000 + period_ns - 1) / period_ns;  // existing A20 grid
  run.advance = static_cast<double>(ticks * period_ns) / 1e9;   // T - S
  auto& world = *run.world;
  world.config.stop_traj_delta_s = lead;
  auto lane = world.port.ReadLaneFacts();
  lane.supports_scheduled_replacement = true;
  lane.controller_update_rate_hz = rate;
  lane.experiment.midtick_stamp = true;
  world.port.SetLaneFacts(lane);
  world.frozen = factor;
  world.Build();
  world.Dispatch(run.stage);
  world.Sample(0.5, 0.5, 1.0, factor);
  run.source = *world.port.ReadSample();
  run.lead_end = run.source.cursor_s + factor * lead;
  run.c = run.source.cursor_s + factor * run.advance + run.period;
  ScriptReplacement(world, kSecond, 5U, ExecutionGoalResult::kSucceeded);
  auto budget = Budget(run.stage);
  if (remaining) budget.remaining_controlled_deadline_ns = *remaining;
  run.result.emplace(world.helper->TranslateStopStage(run.stage, budget, {}));
  return run;
}

JsonObjectDTO ActualCParameters(const char* mode, double deceleration, double duration,
                                std::optional<double> jerk = std::nullopt) {
  auto parameters = Parameters(mode, false, std::nullopt, deceleration);
  parameters.insert_or_assign("deceleration_time_s", JsonValueDTO{duration});
  if (jerk) {
    parameters.insert_or_assign("jerk_limits_enabled", JsonValueDTO{true});
    auto& row = std::get<JsonObjectDTO>(
        std::get<JsonValueDTO::Array>(parameters.at("per_resource_limits").value).front().value);
    row.emplace("max_jerk", JsonValueDTO{JsonObjectDTO{
                                {"coordinate_kind", JsonValueDTO{std::string("angular_rad_s3")}},
                                {"value_rad_s3", JsonValueDTO{*jerk}}}});
  }
  return parameters;
}

void ExpectHardTakesOver(ActualCRun& run, const std::string& reason) {
  ASSERT_EQ(run.result->outcome, ParticipantStopStageOutcomeDTO::kFailed);
  ASSERT_TRUE(run.result->error);
  EXPECT_EQ(run.result->error->details.at("reason"), reason);
  EXPECT_EQ(run.result->error->details.at("recommended_disposition"), "hard_stop");
  EXPECT_TRUE(Replaces(run.world->port).empty());
  auto& world = *run.world;
  ScriptBrake(world, kSecond, 4U, world.frozen);
  run.stage.stage_index = 1U;  // existing Kernel fallback owner selects the hard stage
  const auto hard = world.helper->TranslateStopStage(run.stage, Budget(run.stage), {});
  ASSERT_EQ(hard.outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
      << (hard.error ? hard.error->message : "");
  const auto brakes = Replaces(world.port);
  ASSERT_EQ(brakes.size(), 1U);
  EXPECT_NEAR(brakes.front().trajectory.waypoints.front().positions_rad[0], run.c, 1e-10);
}

// ACTUAL-C: C < L keeps the unchanged law; C >= L
// (including C == L and the top of the eligible range, tau = C - L near
// P(1 - f/2)) re-plans from source(C): first command source(C), no catch-up
// against the source commands already issued, original spring-return target,
// law within its caps, and the soft stop point moved by exactly (C - L)/2.
TEST(ParticipantStopStageHelperTest, ActualCReplansSoftAndHoldFromSourceAtTakeover) {
  constexpr double kDuration = 1.0;
  constexpr double kDeceleration = 10.0;
  for (const double rate : {100.0, 150.0}) for (const double factor : {1.0, 0.5, 0.1})
    for (const char* mode : {"soft_on_path", "spring_back_hold"})
      for (const char* place : {"before", "equal", "inside", "top"}) {
        SCOPED_TRACE(::testing::Message() << rate << ' ' << factor << ' ' << mode << ' ' << place);
        const double period = static_cast<double>(static_cast<Duration>(1e9 / rate)) / 1e9;
        const double advance = std::ceil(0.02 / period - 1e-9) * period;
        // lead: L before C by >= P/2 ("before"), at C, C - L = 0.1 P, or E just before L.
        const double lead = std::string(place) == "before"  ? advance + 1.5 * period / factor
                            : std::string(place) == "equal" ? advance + period / factor
                            : std::string(place) == "inside"
                                ? advance + 0.9 * period / factor
                                : advance + 0.55 * period;
        auto run = RunActualC(rate, factor, lead,
                              ActualCParameters(mode, kDeceleration, kDuration));
        ASSERT_EQ(run.result->outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
            << (run.result->error ? run.result->error->message : "");
        const auto calls = Replaces(run.world->port);
        ASSERT_EQ(calls.size(), 1U);
        const auto& sent = calls.front().trajectory;
        const double r = sent.waypoints.front().time_from_start_s;
        const double tau = run.c - run.lead_end;
        if (std::string(place) == "before") {
          ASSERT_LT(tau, 0.0);
          // Unchanged law: the first knot is the source at C (C < L), and the
          // law still leaves the source exactly at L.
          EXPECT_NEAR(sent.waypoints.front().positions_rad[0], run.c, 1e-10);
          continue;
        }
        if (std::string(place) == "equal") {
          EXPECT_NEAR(tau, 0.0, 1e-12);
        }
        if (std::string(place) == "top") {
          EXPECT_GT(tau, 0.0);
          EXPECT_LT(tau, period * (1.0 - factor / 2.0));
        }
        const auto handover = ExpectActualC(sent, run.c, r, factor, period);
        for (int i = 0; i < 2; ++i) {
          // No catch-up: the finite-difference acceleration at the handover is
          // the law's own analytic acceleration within O(jerk P), and in cap.
          EXPECT_NEAR(handover.fd_a[i], handover.analytic_a[i], 0.05) << i;
          EXPECT_LE(std::abs(handover.fd_a[i]), kDeceleration) << i;
        }
        EXPECT_LE(PeakKnotDeceleration(sent, factor), kDeceleration + 1e-9);
        const auto label = std::string("actual_c_") + mode + '_' +
                           std::to_string(static_cast<int>(rate)) + '_' + std::to_string(factor) +
                           '_' + place;
        std::ostringstream numbers;
        numbers << std::setprecision(17) << "tau_path=" << tau << ",fd_a0=" << handover.fd_a[0]
                << ",fd_a1=" << handover.fd_a[1] << ",fd_j0=" << handover.fd_j[0]
                << ",fd_j1=" << handover.fd_j[1] << ",final=" << sent.waypoints.back().positions_rad[0];
        const double end = sent.waypoints.back().positions_rad[0];
        if (std::string(mode) == "spring_back_hold") {
          // The return target stays the ORIGINAL seam (trip cursor), not C.
          EXPECT_NEAR(end, run.source.cursor_s, 1e-12);
        } else {
          // Unchanged law: L + f(D - lead)/2. Re-planned: C + f(D - lead')/2,
          // lead' = lead + tau/f, i.e. later by exactly tau/2 (path = rad here).
          const double unchanged = run.lead_end + 0.5 * factor * (kDuration - lead);
          EXPECT_NEAR(end - unchanged, 0.5 * tau, 1e-9);
          numbers << ",stop_shift=" << end - unchanged;
        }
        RecordProperty(label, numbers.str());
        EXPECT_TRUE(Cancels(run.world->port).empty());
      }
}

// ACTUAL-C with authored jerk limits enabled, at the top of the eligible range
// for f = 0.1: the re-planned law passes the planner's own analytic segment
// jerk check; the handover finite differences are reported and stay in caps.
TEST(ParticipantStopStageHelperTest, ActualCReplanKeepsAuthoredJerkLimits) {
  constexpr double kJerk = 50.0;
  for (const double rate : {100.0, 150.0}) for (const char* mode : {"soft_on_path", "spring_back_hold"}) {
    SCOPED_TRACE(::testing::Message() << rate << ' ' << mode);
    const double period = static_cast<double>(static_cast<Duration>(1e9 / rate)) / 1e9;
    const double advance = std::ceil(0.02 / period - 1e-9) * period;
    auto run = RunActualC(rate, 0.1, advance + 0.55 * period,
                          ActualCParameters(mode, 10.0, 1.0, kJerk));
    ASSERT_EQ(run.result->outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
        << (run.result->error ? run.result->error->message : "");
    ASSERT_GT(run.c, run.lead_end);
    const auto calls = Replaces(run.world->port);
    ASSERT_EQ(calls.size(), 1U);
    const auto& sent = calls.front().trajectory;
    const auto handover =
        ExpectActualC(sent, run.c, sent.waypoints.front().time_from_start_s, 0.1, period);
    for (int i = 0; i < 2; ++i) {
      EXPECT_LE(std::abs(handover.fd_a[i]), 10.0) << i;
      EXPECT_LE(std::abs(handover.fd_j[i]), kJerk) << i;  // finite difference, not analytic
    }
    EXPECT_LE(PeakKnotDeceleration(sent, 0.1), 10.0 + 1e-9);
  }
}

// ACTUAL-C: a re-plan that no longer fits refuses with the existing reasons and
// the existing hard stage takes over; the same C just inside the boundary sends.
TEST(ParticipantStopStageHelperTest, ActualCReplanOutsideTheRemainingBudgetFallsBackToHard) {
  constexpr double kRate = 100.0;
  constexpr double kFactor = 0.1;
  const double period = 0.01;
  const double advance = 0.02;
  const double lead = advance + 0.55 * period;  // C - L = P - 0.55 f P
  const double replanned_lead = advance + period / kFactor;
  // (1) Deceleration cap: the unchanged soft ramp (D - lead) fits 5 rad/s^2,
  // the re-planned ramp (D - lead') does not.
  {
    auto run = RunActualC(kRate, kFactor, lead,
                          ActualCParameters("soft_on_path", 5.0, 0.14));
    ASSERT_GT(run.c, run.lead_end);
    ExpectHardTakesOver(run, "soft_on_path_acceleration_limit_exceeded");
  }
  // (2) Hold forward half: just below / above the re-planned lead-in.
  for (const double half : {replanned_lead - 0.001, replanned_lead + 0.002}) {
    SCOPED_TRACE(half);
    auto run = RunActualC(kRate, kFactor, lead,
                          ActualCParameters("spring_back_hold", 1000.0, 2.0 * half));
    if (half < replanned_lead) {
      ExpectHardTakesOver(run, "spring_back_phase_time_not_larger_than_handoff");
      continue;
    }
    ASSERT_EQ(run.result->outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
        << (run.result->error ? run.result->error->message : "");
    const auto calls = Replaces(run.world->port);
    ASSERT_EQ(calls.size(), 1U);
    const auto& sent = calls.front().trajectory;
    (void)ExpectActualC(sent, run.c, sent.waypoints.front().time_from_start_s, kFactor, period);
  }
  // (3) Absolute deadline from the ORIGINAL request: the law ends at
  // T + (D - lead'); one ms short of it refuses, one ms more sends.
  for (const char* mode : {"soft_on_path", "spring_back_hold"}) for (const Duration slack : {-1'000'000, 1'000'000}) {
    SCOPED_TRACE(::testing::Message() << mode << ' ' << slack);
    const auto need = static_cast<Duration>(std::llround((advance + 1.0 - replanned_lead) * 1e9));
    auto run = RunActualC(kRate, kFactor, lead, ActualCParameters(mode, 10.0, 1.0),
                          need + slack);
    if (slack < 0) {
      ExpectHardTakesOver(run, std::string(mode == std::string("soft_on_path") ? "soft_on_path"
                                                                               : "spring_back") +
                                   "_stop_delta_budget_exceeded");
      continue;
    }
    ASSERT_EQ(run.result->outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
        << (run.result->error ? run.result->error->message : "");
  }
}

TEST(ParticipantStopStageHelperTest, ScheduledShortBrakeAndPastEndKeepTheWholeExistingLaw) {
  for (const double speed : {0.0, 0.001, 1.0}) {
    World world;
    auto lane = world.port.ReadLaneFacts();
    lane.supports_scheduled_replacement = true;
    world.port.SetLaneFacts(lane);
    const auto stage = Stage(1U);
    auto goal = Goal(stage);
    for (auto& point : goal.trajectory.waypoints) {
      point.positions_rad[0] *= speed;
      point.velocities_rad_s[0] *= speed;
    }
    world.helper->BindDispatchedGoal(goal, std::make_shared<ExecutionSampleAssociation>(
        ExecutionBindingEpoch(1U), std::nullopt, world.clock.now()), TestCancelAttempt());
    world.Executing(kOriginal);
    const double cursor = speed == 0.0 ? 2.0 : 0.5;
    world.Sample(cursor, speed * cursor, speed);
    ScriptBrake(world, kSecond);
    const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
    ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded);
    const auto points = Replaces(world.port).front().trajectory.waypoints;
    EXPECT_NEAR(points.front().time_from_start_s, 0.004, 1e-12);
    EXPECT_NEAR(points.front().positions_rad[0], speed * (cursor + 0.014), 1e-12);
    EXPECT_NEAR(points.back().time_from_start_s - points.front().time_from_start_s,
                0.5 * (0.5 * speed / 4.0), 1e-12);
    EXPECT_TRUE(Cancels(world.port).empty());
  }
}

TEST(ParticipantStopStageHelperTest, ScheduledMissedHardReleaseUsesTheExistingCancelFloor) {
  World world(false);
  world.config.replacement_start_allowance_s = 0.0;
  world.Build();
  auto lane = world.port.ReadLaneFacts();
  lane.supports_scheduled_replacement = true;
  world.port.SetLaneFacts(lane);
  const auto stage = Stage(1U);
  world.Dispatch(stage);
  world.Sample(0.5, 0.5, 1.0);
  world.on_tick = [&] {
    if (Cancels(world.port).empty()) return;
    world.port.MarkCancelSent(kOriginal, world.clock.now() - 1);
    world.Cancelled(kOriginal);
    world.Sample(0.001, 0.5, 0.0);
  };
  const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
  ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded);
  EXPECT_TRUE(Replaces(world.port).empty());
  EXPECT_EQ(Cancels(world.port), std::vector<ExecutionGoalKey>{kOriginal});
  EXPECT_EQ(Text(EvidenceObject(result), "hard_stop_fallback_reason"),
            "controlled_hard_stop_release_expired");
}

TEST(ParticipantStopStageHelperTest, EarlierRefusalDoesNotConvertHardHoldTerminationToFallback) {
  for (const bool cancel : {false, true}) {
    World world(false);
    world.config.stop_traj_delta_s = 0.019;  // LEAD-B: a genuinely late start still refuses
    if (!cancel) world.config.controller_state_max_age_ns = 5'000'000;
    world.Build();
    auto lane = world.port.ReadLaneFacts();
    lane.supports_scheduled_replacement = true;
    world.port.SetLaneFacts(lane);
    auto stage = Stage(0U, Nominal::kControlled, Parameters("soft_on_path", false));
    world.Dispatch(stage);
    world.Sample(0.5, 0.5, 1.0);
    const auto refused = world.helper->TranslateStopStage(stage, Budget(stage), {});
    ASSERT_EQ(refused.outcome, ParticipantStopStageOutcomeDTO::kFailed);
    ASSERT_TRUE(refused.error);
    EXPECT_EQ(refused.error->details.at("recommended_disposition"), "hard_stop");
    std::stop_source cancellation;
    world.on_tick = [&] { if (cancel) cancellation.request_stop(); };
    stage.stage_index = 1U;
    const auto result = world.helper->TranslateStopStage(stage, Budget(stage), cancellation.get_token());
    EXPECT_EQ(result.outcome, cancel ? ParticipantStopStageOutcomeDTO::kSupersededNoEffect
                                    : ParticipantStopStageOutcomeDTO::kFailed);
    if (!cancel) {
      ASSERT_TRUE(result.error);
      EXPECT_EQ(result.error->details.at("reason"), "stale_controller_state");
    }
    EXPECT_TRUE(Replaces(world.port).empty());
    EXPECT_TRUE(Cancels(world.port).empty());
  }
}

TEST(ParticipantStopStageHelperTest, SourceOffsetSurvivesBindingRecordingAndAdoption) {
  for (const bool adopted : {false, true}) {
    World world;
    auto lane = world.port.ReadLaneFacts();
    lane.supports_scheduled_replacement = true;
    world.port.SetLaneFacts(lane);
    auto stage = Stage(1U);
    world.Dispatch(stage);
    auto goal = Goal(stage);
    goal.planned_controller_offset_s = 0.001;
    const auto association = std::make_shared<ExecutionSampleAssociation>(
        ExecutionBindingEpoch(1U), std::nullopt, world.clock.now());
    if (adopted) {
      ASSERT_TRUE(world.helper->ReserveReplacement(kOriginal));
      goal.key = kSecond;
      world.helper->RecordReplacement(kSecond, goal.trajectory, association, 0.001,
                                      TestCancelAttempt());
      world.port.FinishGoal(kOriginal, ExecutionGoalResult::kPreemptedBy, kSecond, world.clock.now());
      std::optional<StopStageDispatchedGoalDTO> candidate(goal);
      world.helper->AdoptReplacement(candidate);
      stage.controller_goal_generation = kSecond.controller_goal_generation;
      world.Executing(kSecond);
    } else {
      world.helper->BindDispatchedGoal(goal, association, TestCancelAttempt());
    }
    world.Sample(0.002, 0.001, 1.0);  // bridge guard: planned offset must be retained
    ScriptBrake(world, adopted ? kThird : kSecond);
    const auto result = world.helper->TranslateStopStage(stage, Budget(stage), {});
    ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
        << (result.error ? result.error->message : "");
    EXPECT_NEAR(Replaces(world.port).front().trajectory.waypoints.front().positions_rad[0],
                0.001 + 0.5 * 0.020 + 0.004, 1e-12);
  }
}


// Original Kernel ingress is intentionally earlier than the helper starts.
// The receiver clock epoch is arbitrary; only the relative age is carried.
TEST(ParticipantStopStageHelperTest, SpringReturnTargetsOriginalIngressAcrossRatesFactorsAndDelay) {
  for (const double rate : {100.0, 150.0})
    for (const double factor : {1.0, 0.5, 0.1})
      for (const double phase : {0.0, 0.5, 1.0})
        for (const char* mode : {"spring_back_fast", "spring_back_hold"}) {
          SCOPED_TRACE(::testing::Message() << rate << ' ' << factor << ' ' << phase << ' ' << mode);
          World world(false);
          world.clock.set(71'000'000'000);  // not the Kernel's epoch
          const auto period_ns = static_cast<Duration>(1e9 / rate);
          const double period = static_cast<double>(period_ns) / 1e9;
          const auto advance_ns = ((20'000'000 + period_ns - 1) / period_ns) * period_ns;
          world.config.stop_traj_delta_s = static_cast<double>(advance_ns) / 1e9 + 0.55 * period;
          const auto age = static_cast<Duration>(phase * period_ns);
          const double old_target = 0.5 + factor * static_cast<double>(age) / 1e9;
          const double expected = 0.5 + period;
          auto lane = world.port.ReadLaneFacts();
          lane.supports_scheduled_replacement = true;
          lane.controller_update_rate_hz = rate;
          lane.experiment.midtick_stamp = true;
          world.port.SetLaneFacts(lane);
          world.frozen = factor;
          world.Build();
          const auto stage = Stage(0U, Nominal::kControlled,
                                   ActualCParameters(mode, 10.0, 1.0));
          world.Dispatch(stage);
          world.clock.advance(age);
          world.Sample(old_target, old_target, 1.0, factor);
          ScriptReplacement(world, kSecond, 5U, ExecutionGoalResult::kSucceeded);
          SpringPointObserver observer(world);
          auto budget = Budget(stage);
          budget.stop_ingress_age_ns = age;
          // Keep the original deadline values: the new age is target-only.
          const auto original_elapsed = budget.elapsed_since_stop_ns;
          const auto original_deadline = budget.remaining_controlled_deadline_ns;
          const auto result = world.helper->TranslateStopStageForTakeover(
              stage, budget, {}, observer);
          ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
              << (result.error ? result.error->message : "");
          const auto calls = Replaces(world.port);
          ASSERT_EQ(calls.size(), 1U);
          EXPECT_NEAR(calls.front().trajectory.waypoints.back().positions_rad[0], expected, 2e-8);
          ASSERT_TRUE(observer.point);
          ASSERT_TRUE(observer.point->initial.spring_back_return_path_time_s);
          EXPECT_NEAR(*observer.point->initial.spring_back_return_path_time_s, expected, 2e-8);
          EXPECT_NEAR(observer.point->initial.spring_back_return_point.positions_rad[0], expected, 2e-8);
          EXPECT_EQ(budget.elapsed_since_stop_ns, original_elapsed);
          EXPECT_EQ(budget.remaining_controlled_deadline_ns, original_deadline);
          EXPECT_TRUE(Cancels(world.port).empty());
          std::ostringstream numbers;
          numbers << std::setprecision(17) << "arrival_to_sample_ns=" << age
                  << ",old_target=" << old_target << ",new_target=" << expected
                  << ",delta=" << expected - old_target;
          const auto label = std::string(mode) + '_' + std::to_string(static_cast<int>(rate)) +
                             '_' + std::to_string(factor) + "_arrival_" + std::to_string(age);
          RecordProperty(label + "_result", numbers.str());
          ASSERT_TRUE(calls.front().start);
          const auto& start = *calls.front().start;
          ASSERT_TRUE(start.experiment);
          const auto source = 0.5 + factor * static_cast<double>(age) / 1e9;
          const double c = source + factor * static_cast<double>(
              start.experiment->target_tick_ns - (71'000'000'000 + age)) / 1e9 + period;
          std::ostringstream metadata;
          metadata << std::setprecision(17) << rate << ',' << factor << ','
                   << start.controller_start_stamp_ns << ',' << start.experiment->target_tick_ns
                   << ',' << 71'000'000'000 + age << ',' << source << ',' << c << ','
                   << calls.front().trajectory.waypoints.front().time_from_start_s << ",0";
          RecordProperty(label + "_metadata", metadata.str());
          RecordProperty(label + "_joints", "joint_1");
          RecordProperty(label + "_source", ReceiverCurve(Original()));
          RecordProperty(label + "_sent", ReceiverCurve(calls.front().trajectory));
        }
}

TEST(ParticipantStopStageHelperTest, FastSpringNearEndFollowsNaturalCompletionSetting) {
  for (const bool natural : {true, false}) {
    SCOPED_TRACE(natural);
    World world;
    const auto stage = Stage(0U, Nominal::kControlled, Parameters("spring_back_fast", natural));
    world.Dispatch(stage);
    world.clock.advance(20'000'000);
    world.Sample(1.9, 1.9, 1.0);
    auto budget = Budget(stage);
    budget.stop_ingress_age_ns = 20'000'000;
    if (natural) {
      world.on_tick = [&] {
        if (world.ticks == 5U) world.Finish(kOriginal, ExecutionGoalResult::kSucceeded);
        world.Sample(1.95, 1.95, world.ticks >= 5U ? 0.0 : 1.0);
      };
    } else {
      ScriptReplacement(world, kSecond, 5U, ExecutionGoalResult::kSucceeded);
    }
    const auto result = world.helper->TranslateStopStage(stage, budget, {});
    ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
        << (result.error ? result.error->message : "");
    const auto calls = Replaces(world.port);
    if (natural) {
      EXPECT_TRUE(calls.empty());
      EXPECT_TRUE(std::get<bool>(EvidenceObject(result).at("completed_by_natural_completion").value));
    } else {
      ASSERT_EQ(calls.size(), 1U);
      EXPECT_NEAR(calls.front().trajectory.waypoints.back().positions_rad[0], 1.89, 2e-8);
      EXPECT_FALSE(std::get<bool>(EvidenceObject(result).at("completed_by_natural_completion").value));
    }
  }
}


TEST(ParticipantStopStageHelperTest, InvalidIngressAgeRejectsBeforeAnyControlledEffect) {
  for (const auto age : {std::int64_t{-1}, kMaximumStopDurationNs + 1}) {
    SCOPED_TRACE(age);
    World world;
    const auto stage = Stage(0U, Nominal::kControlled, Parameters("spring_back_fast", false));
    world.Dispatch(stage);
    world.Sample(0.5, 0.5, 1.0);
    auto budget = Budget(stage);
    budget.stop_ingress_age_ns = age;
    const auto commands = world.CommandCount();
    const auto result = world.helper->TranslateStopStage(stage, budget, {});
    EXPECT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kFailed);
    ASSERT_TRUE(result.error);
    EXPECT_EQ(result.error->details.at("reason"), "resolved_stop_stage_budget_invalid");
    EXPECT_EQ(world.CommandCount(), commands);
    EXPECT_EQ(world.freezes, 0U);
  }
}


TEST(ParticipantStopStageHelperTest, ScheduledNearEndSpringRespectsSettingAndExistingFeasibility) {
  for (const double rate : {100.0, 150.0})
    for (const char* mode : {"spring_back_fast", "spring_back_hold"})
      for (const bool natural : {false, true}) {
        SCOPED_TRACE(::testing::Message() << rate << ' ' << mode << ' ' << natural);
        World world;
        auto lane = world.port.ReadLaneFacts();
        lane.supports_scheduled_replacement = true;
        lane.controller_update_rate_hz = rate;
        world.port.SetLaneFacts(lane);
        const double period = static_cast<double>(static_cast<Duration>(1e9 / rate)) / 1e9;
        const auto stage = Stage(0U, Nominal::kControlled, Parameters(mode, natural));
        world.Dispatch(stage);
        world.clock.advance(20'000'000);
        world.Sample(1.9, 1.9, 1.0);
        auto budget = Budget(stage);
        budget.stop_ingress_age_ns = 20'000'000;
        if (natural) {
          world.on_tick = [&] {
            if (world.ticks == 5U) world.Finish(kOriginal, ExecutionGoalResult::kSucceeded);
            world.Sample(1.95, 1.95, world.ticks >= 5U ? 0.0 : 1.0);
          };
        } else {
          ScriptReplacement(world, kSecond, 5U, ExecutionGoalResult::kSucceeded);
        }
        const auto result = world.helper->TranslateStopStage(stage, budget, {});
        const auto calls = Replaces(world.port);
        if (std::string(mode) == "spring_back_hold") {
          // Existing limit-respecting hold cannot brake on this short source.
          // Its planner recommends hard Stop for either setting; target-only
          // work must neither bypass that check nor fabricate a successful return.
          EXPECT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kFailed);
          ASSERT_TRUE(result.error);
          EXPECT_EQ(result.error->details.at("recommended_disposition"), "hard_stop");
          EXPECT_TRUE(calls.empty());
        } else if (natural) {
          ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded);
          EXPECT_TRUE(calls.empty());
          EXPECT_TRUE(std::get<bool>(EvidenceObject(result).at("completed_by_natural_completion").value));
        } else {
          ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
              << (result.error ? result.error->message : "");
          ASSERT_EQ(calls.size(), 1U);
          EXPECT_NEAR(calls.front().trajectory.waypoints.back().positions_rad[0], 1.89 + period, 2e-8);
          EXPECT_FALSE(std::get<bool>(EvidenceObject(result).at("completed_by_natural_completion").value));
        }
      }
}

TEST(ParticipantStopStageHelperTest, SpringArrivalTargetSubtractsDeliveryOffsetOnlyOnce) {
  for (const char* mode : {"spring_back_fast", "spring_back_hold"}) {
    SCOPED_TRACE(mode);
    World world;
    auto lane = world.port.ReadLaneFacts();
    lane.supports_scheduled_replacement = true;
    lane.controller_update_rate_hz = 100.0;
    world.port.SetLaneFacts(lane);
    const auto stage = Stage(0U, Nominal::kControlled, ActualCParameters(mode, 10.0, 1.0));
    auto goal = Goal(stage);
    goal.planned_controller_offset_s = 0.02;
    world.helper->BindDispatchedGoal(goal, std::make_shared<ExecutionSampleAssociation>(
        ExecutionBindingEpoch(1U), world.port.ReadSample(), world.clock.now()), TestCancelAttempt());
    world.Executing(kOriginal);
    world.clock.advance(5'000'000);
    world.Sample(0.52, 0.5, 1.0);
    ScriptReplacement(world, kSecond, 5U, ExecutionGoalResult::kSucceeded);
    SpringPointObserver observer(world);
    auto budget = Budget(stage);
    budget.stop_ingress_age_ns = 5'000'000;
    const auto result = world.helper->TranslateStopStageForTakeover(stage, budget, {}, observer);
    ASSERT_EQ(result.outcome, ParticipantStopStageOutcomeDTO::kSucceeded)
        << (result.error ? result.error->message : "");
    ASSERT_TRUE(observer.point);
    EXPECT_NEAR(observer.point->initial.spring_back_return_path_time_s.value(), 0.5075, 2e-8);
    const auto calls = Replaces(world.port);
    ASSERT_EQ(calls.size(), 1U);
    EXPECT_NEAR(calls.front().trajectory.waypoints.back().positions_rad[0], 0.5075, 2e-8);
  }
}

}  // namespace
}  // namespace manyforge::core
