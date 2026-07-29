// Copyright 2026 Flexin Group SRL
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <manyforge_xarm_control/coordinator.hpp>
#include <manyforge_xarm_control/fake_hardware_backend.hpp>
#include <manyforge_xarm_control/lite6_lifecycle_catalog.hpp>

namespace manyforge::xarm_control
{
namespace
{

constexpr std::int64_t kNow = 10'000'000'000;
constexpr std::int64_t kLeaseNs = 1'000'000'000;
constexpr std::size_t kJointCount = 6;
constexpr std::size_t kConcurrentRtIterations = 2'000;
constexpr std::int64_t kConcurrentNrtIterations = 500;

struct TestRig
{
  std::unique_ptr<XArmLifecycleCoordinator> coordinator;
  FakeXArmHardwareBackend * backend = nullptr;
};

class DeferredFakeXArmHardwareBackend final : public IXArmHardwareBackend
{
public:
  struct Work
  {
    XArmLifecycleCommand command = XArmLifecycleCommand::kClearError;
    std::string request_id;
    std::uint64_t expected_generation = 0;
  };

  struct Completion
  {
    Work work;
    XArmLifecycleCommandResult result;
  };

  DeferredFakeXArmHardwareBackend()
  : backend_("deferred-fake-owner", kJointCount, kLeaseNs)
  {
    backend_.set_enable_motion_implies_vendor_ready(false);
  }

  [[nodiscard]] std::string_view owner_id() const noexcept override
  {
    return backend_.owner_id();
  }

  [[nodiscard]] XArmObservedState observe(
    std::int64_t now_ns) const override
  {
    return backend_.observe(now_ns);
  }

  XArmLifecycleCommandResult execute_lifecycle(
    XArmLifecycleCommand command,
    std::string_view request_id,
    std::uint64_t expected_generation) override
  {
    XArmLifecycleCommandResult result;
    result.observed_generation = expected_generation;
    if (queued_.has_value()) {
      result.reason = "deferred_fake_worker_busy";
      return result;
    }
    queued_ = Work{
      command, std::string(request_id), expected_generation};
    result.state = XArmLifecycleResultState::kExecutionDeferred;
    result.reason = "deferred_fake_worker_accepted";
    return result;
  }

  bool read_joint_state(JointStateSample & sample) override
  {
    return backend_.read_joint_state(sample);
  }

  bool write_joint_command(const JointCommandView & command) override
  {
    return backend_.write_joint_command(command);
  }

  XArmCommandGateUpdate set_command_gate(
    bool open, std::int64_t valid_until_ns) override
  {
    return backend_.set_command_gate(open, valid_until_ns);
  }

  void set_hardware_component_active(bool active) override
  {
    backend_.set_hardware_component_active(active);
  }

  void set_trajectory_controller_active(bool active) override
  {
    backend_.set_trajectory_controller_active(active);
  }

  [[nodiscard]] bool work_queued() const noexcept
  {
    return queued_.has_value();
  }

  Completion execute_queued()
  {
    if (!queued_.has_value()) {
      throw std::logic_error("deferred fake has no queued work");
    }
    Work work = std::move(*queued_);
    queued_.reset();
    auto result = backend_.execute_lifecycle(
      work.command, work.request_id, work.expected_generation);
    return {std::move(work), std::move(result)};
  }

  [[nodiscard]] std::vector<XArmLifecycleCommand> command_log() const
  {
    return backend_.command_log();
  }

private:
  FakeXArmHardwareBackend backend_;
  std::optional<Work> queued_;
};

struct DeferredTestRig
{
  std::unique_ptr<XArmLifecycleCoordinator> coordinator;
  DeferredFakeXArmHardwareBackend * backend = nullptr;
};

DeferredTestRig make_deferred_target_rig()
{
  auto backend = std::make_unique<DeferredFakeXArmHardwareBackend>();
  auto * backend_view = backend.get();
  robot_lifecycle::SupervisorConfig config;
  config.robot_id = "lite6-deferred";
  config.supervisor_instance_id = "lite6-deferred:test-supervisor";
  config.authority_profile =
    robot_lifecycle::AuthorityProfile::kLocalOperator;
  auto coordinator = std::make_unique<XArmLifecycleCoordinator>(
    std::move(config), std::move(backend),
    make_lite6_xarm_action_plans(), kLeaseNs, 128,
    make_lite6_xarm_target_plans());
  return {std::move(coordinator), backend_view};
}

TestRig make_rig(
  std::string robot_id = "lite6-a",
  std::string owner_id = "transport-owner-a",
  std::vector<XArmActionPlan> action_plans =
  make_fake_xarm_action_plans(),
  std::size_t primitive_audit_capacity = 128,
  std::vector<XArmTargetPlan> target_plans = {})
{
  auto backend = std::make_unique<FakeXArmHardwareBackend>(
    std::move(owner_id), kJointCount, kLeaseNs);
  auto * backend_view = backend.get();
  robot_lifecycle::SupervisorConfig config;
  config.robot_id = std::move(robot_id);
  config.supervisor_instance_id = config.robot_id + ":test-supervisor";
  config.authority_profile =
    robot_lifecycle::AuthorityProfile::kLocalOperator;
  auto coordinator = std::make_unique<XArmLifecycleCoordinator>(
    std::move(config), std::move(backend),
    std::move(action_plans), kLeaseNs, primitive_audit_capacity,
    std::move(target_plans));
  return {std::move(coordinator), backend_view};
}

TestRig make_target_rig()
{
  auto rig = make_rig(
    "lite6-a", "transport-owner-a",
    make_lite6_xarm_action_plans(), 128,
    make_lite6_xarm_target_plans());
  rig.backend->set_enable_motion_implies_vendor_ready(false);
  return rig;
}

robot_lifecycle::AuthoritySession authority(
  std::string session_id = "operator-session")
{
  robot_lifecycle::AuthoritySession session;
  session.profile = robot_lifecycle::AuthorityProfile::kLocalOperator;
  session.session_id = std::move(session_id);
  session.generation = 1;
  session.principal_id = "human:operator";
  session.valid_until_ns = kNow + kLeaseNs;
  session.component_epoch = "operator-epoch-1";
  return session;
}

robot_lifecycle::CommandRequest request_for(
  const robot_lifecycle::LifecycleSnapshot & snapshot,
  std::string action_id = "robot.enable",
  std::uint64_t sequence = 1,
  std::string request_id = "request-1")
{
  robot_lifecycle::CommandRequest request;
  request.robot_id = snapshot.robot_id;
  request.request_id = std::move(request_id);
  request.request_sequence = sequence;
  request.action_id = std::move(action_id);
  request.authority_profile = snapshot.authority_profile;
  request.authority_session_id = snapshot.authority_session->session_id;
  request.authority_session_generation =
    snapshot.authority_session->generation;
  request.principal_id = snapshot.authority_session->principal_id;
  request.expected_snapshot_generation = snapshot.generation;
  request.confirmation_evidence = "test-confirmation";
  return request;
}

void establish_ready_authority(TestRig & rig)
{
  ASSERT_TRUE(rig.coordinator->refresh(kNow));
  ASSERT_TRUE(rig.coordinator->establish_authority_session(
    authority(), kNow));
}

TEST(EndToEndFakeTest, ExecutesOnceAndTerminatesOnObservedPostcondition)
{
  auto rig = make_rig();
  establish_ready_authority(rig);
  const auto request = request_for(rig.coordinator->snapshot(kNow));

  const auto accepted = rig.coordinator->submit(request, kNow + 1);

  EXPECT_EQ(
    accepted.disposition,
    robot_lifecycle::CommandDisposition::kAccepted);
  ASSERT_EQ(rig.backend->command_log().size(), 1U);
  EXPECT_EQ(
    rig.backend->command_log().front(),
    XArmLifecycleCommand::kEnableMotion);
  const auto completed = rig.coordinator->snapshot(kNow + 1);
  EXPECT_FALSE(completed.pending_transaction.has_value());
  ASSERT_TRUE(completed.last_terminal_transaction.has_value());
  EXPECT_EQ(
    completed.last_terminal_transaction->phase,
    robot_lifecycle::TransactionPhase::kSucceeded);
  EXPECT_EQ(
    completed.last_terminal_transaction->result_reason,
    "xarm_postcondition_observed");
  const auto audit = rig.coordinator->primitive_audit_snapshot();
  ASSERT_EQ(audit.size(), 1U);
  EXPECT_EQ(audit.front().sequence, 1U);
  EXPECT_EQ(
    audit.front().transaction_generation,
    accepted.transaction_generation);
  EXPECT_EQ(audit.front().action_id, "robot.enable");
  EXPECT_EQ(audit.front().stage_action_id, "robot.enable");
  EXPECT_EQ(audit.front().step_index, 0U);
  EXPECT_EQ(
    audit.front().transport_request_id,
    "mf:" + request.request_id + ":step:0");
  EXPECT_GT(audit.front().expected_observation_generation, 0U);
  EXPECT_EQ(
    audit.front().command,
    XArmLifecycleCommand::kEnableMotion);
  EXPECT_TRUE(audit.front().result.permitted);
  EXPECT_TRUE(audit.front().result.attempted);
  EXPECT_EQ(audit.front().result.return_code, 0);

  const auto replay = rig.coordinator->submit(request, kNow + 2);
  EXPECT_EQ(
    replay.disposition,
    robot_lifecycle::CommandDisposition::kIdempotentReplay);
  EXPECT_EQ(rig.backend->command_log().size(), 1U);
  EXPECT_EQ(rig.coordinator->primitive_audit_snapshot().size(), 1U);
}

TEST(
  EndToEndFakeTest,
  PrepareTargetAdvancesOneStagePerFreshObservationUnderOneTransaction)
{
  auto rig = make_target_rig();
  establish_ready_authority(rig);
  const auto request = request_for(
    rig.coordinator->snapshot(kNow),
    std::string(robot_lifecycle::kPrepareForMotionActionId));

  const auto accepted = rig.coordinator->submit(request, kNow + 1);

  ASSERT_EQ(
    accepted.disposition,
    robot_lifecycle::CommandDisposition::kAccepted);
  EXPECT_EQ(
    rig.backend->command_log(),
    std::vector<XArmLifecycleCommand>{
        XArmLifecycleCommand::kEnableMotion});
  for (std::int64_t offset = 2; offset <= 6; ++offset) {
    ASSERT_TRUE(rig.coordinator->refresh(kNow + offset));
  }

  EXPECT_EQ(
    rig.backend->command_log(),
    (std::vector<XArmLifecycleCommand>{
        XArmLifecycleCommand::kEnableMotion,
        XArmLifecycleCommand::kSelectJointServoMode,
        XArmLifecycleCommand::kSetReadyState,
        XArmLifecycleCommand::kActivateHardwareComponent,
        XArmLifecycleCommand::kActivateTrajectoryControllers,
      }));
  const auto snapshot = rig.coordinator->snapshot(kNow + 6);
  EXPECT_FALSE(snapshot.pending_transaction.has_value());
  ASSERT_TRUE(snapshot.last_terminal_transaction.has_value());
  EXPECT_EQ(
    snapshot.last_terminal_transaction->action_id,
    robot_lifecycle::kPrepareForMotionActionId);
  EXPECT_EQ(
    snapshot.last_terminal_transaction->phase,
    robot_lifecycle::TransactionPhase::kSucceeded);
  EXPECT_TRUE(snapshot.motion_admitted);

  const auto audit = rig.coordinator->primitive_audit_snapshot();
  ASSERT_EQ(audit.size(), 5U);
  const std::vector<std::string> expected_stages{
    "robot.enable",
    "robot.select_external_control",
    "robot.start_control",
    "robot.activate_hardware",
    "robot.activate_trajectory_controller",
  };
  for (std::size_t index = 0; index < audit.size(); ++index) {
    EXPECT_EQ(
      audit[index].action_id,
      robot_lifecycle::kPrepareForMotionActionId);
    EXPECT_EQ(audit[index].stage_action_id, expected_stages[index]);
    EXPECT_EQ(audit[index].step_index, index);
    EXPECT_EQ(
      audit[index].transaction_generation,
      accepted.transaction_generation);
  }
}

TEST(
  EndToEndFakeTest,
  DeferredPrepareTargetFencesEachCompletionAndAdvancesOnFreshObservation)
{
  auto rig = make_deferred_target_rig();
  ASSERT_TRUE(rig.coordinator->refresh(kNow));
  ASSERT_TRUE(rig.coordinator->establish_authority_session(
      authority(), kNow));
  const auto request = request_for(
    rig.coordinator->snapshot(kNow),
    std::string(robot_lifecycle::kPrepareForMotionActionId));

  const auto accepted = rig.coordinator->submit(request, kNow + 1);

  ASSERT_EQ(
    accepted.disposition,
    robot_lifecycle::CommandDisposition::kAccepted);
  const std::vector<XArmLifecycleCommand> expected{
    XArmLifecycleCommand::kEnableMotion,
    XArmLifecycleCommand::kSelectJointServoMode,
    XArmLifecycleCommand::kSetReadyState,
    XArmLifecycleCommand::kActivateHardwareComponent,
    XArmLifecycleCommand::kActivateTrajectoryControllers,
  };
  for (std::size_t index = 0; index < expected.size(); ++index) {
    ASSERT_TRUE(rig.backend->work_queued());
    auto completion = rig.backend->execute_queued();
    ASSERT_EQ(completion.work.command, expected[index]);
    const std::int64_t completion_ns =
      kNow + 2 + static_cast<std::int64_t>(index) * 2;
    EXPECT_TRUE(rig.coordinator->complete_deferred_primitive(
        completion.work.command, completion.work.request_id,
        completion.work.expected_generation, completion.result,
        completion_ns));
    EXPECT_FALSE(rig.coordinator->complete_deferred_primitive(
        completion.work.command, completion.work.request_id,
        completion.work.expected_generation, completion.result,
        completion_ns));
    EXPECT_FALSE(rig.backend->work_queued());
    ASSERT_TRUE(rig.coordinator->refresh(completion_ns + 1));
  }

  EXPECT_EQ(rig.backend->command_log(), expected);
  const auto completed = rig.coordinator->snapshot(kNow + 12);
  EXPECT_FALSE(completed.pending_transaction.has_value());
  ASSERT_TRUE(completed.last_terminal_transaction.has_value());
  EXPECT_EQ(
    completed.last_terminal_transaction->phase,
    robot_lifecycle::TransactionPhase::kSucceeded);
  EXPECT_EQ(
    completed.last_terminal_transaction->result_reason,
    "xarm_target_postcondition_observed");
  EXPECT_TRUE(completed.motion_admitted);
  const auto audit = rig.coordinator->primitive_audit_snapshot();
  ASSERT_EQ(audit.size(), expected.size());
  for (std::size_t index = 0; index < audit.size(); ++index) {
    EXPECT_EQ(audit[index].step_index, index);
    EXPECT_EQ(audit[index].command, expected[index]);
    EXPECT_EQ(
      audit[index].transaction_generation,
      accepted.transaction_generation);
  }
}

TEST(
  EndToEndFakeTest,
  AlreadyPreparedTargetSucceedsWithoutPrimitiveOrAdmissionChurn)
{
  auto rig = make_target_rig();
  establish_ready_authority(rig);
  auto first = request_for(
    rig.coordinator->snapshot(kNow),
    std::string(robot_lifecycle::kPrepareForMotionActionId));
  ASSERT_EQ(
    rig.coordinator->submit(first, kNow + 1).disposition,
    robot_lifecycle::CommandDisposition::kAccepted);
  for (std::int64_t offset = 2; offset <= 6; ++offset) {
    ASSERT_TRUE(rig.coordinator->refresh(kNow + offset));
  }
  ASSERT_TRUE(rig.coordinator->motion_admitted(kNow + 6));
  const auto command_count = rig.backend->command_log().size();
  const auto gate_transition_count =
    rig.backend->command_gate_transition_count();

  auto repeated = request_for(
    rig.coordinator->snapshot(kNow + 6),
    std::string(robot_lifecycle::kPrepareForMotionActionId), 2,
    "request-2");
  const auto accepted = rig.coordinator->submit(repeated, kNow + 7);

  EXPECT_EQ(
    accepted.disposition,
    robot_lifecycle::CommandDisposition::kAccepted);
  EXPECT_EQ(rig.backend->command_log().size(), command_count);
  EXPECT_EQ(
    rig.backend->command_gate_transition_count(),
    gate_transition_count);
  EXPECT_TRUE(rig.coordinator->motion_admitted(kNow + 7));
  const auto snapshot = rig.coordinator->snapshot(kNow + 7);
  ASSERT_TRUE(snapshot.last_terminal_transaction.has_value());
  EXPECT_EQ(
    snapshot.last_terminal_transaction->result_reason,
    "xarm_target_already_satisfied");
}

TEST(
  EndToEndFakeTest,
  PrepareTargetAfterAuthorityExpiryRearmsOnlyTheTrajectoryController)
{
  auto rig = make_target_rig();
  establish_ready_authority(rig);
  auto first = request_for(
    rig.coordinator->snapshot(kNow),
    std::string(robot_lifecycle::kPrepareForMotionActionId));
  ASSERT_EQ(
    rig.coordinator->submit(first, kNow + 1).disposition,
    robot_lifecycle::CommandDisposition::kAccepted);
  for (std::int64_t offset = 2; offset <= 6; ++offset) {
    ASSERT_TRUE(rig.coordinator->refresh(kNow + offset));
  }
  ASSERT_TRUE(rig.coordinator->motion_admitted(kNow + 6));
  const auto prepared_commands = rig.backend->command_log();
  ASSERT_EQ(prepared_commands.size(), 5U);

  ASSERT_TRUE(rig.coordinator->refresh(kNow + kLeaseNs));
  EXPECT_FALSE(rig.coordinator->motion_admitted(kNow + kLeaseNs));
  auto expired = rig.coordinator->snapshot(kNow + kLeaseNs);
  EXPECT_NE(
    std::find(
      expired.admission_reason_codes.begin(),
      expired.admission_reason_codes.end(),
      "trajectory_controller_rearm_required"),
    expired.admission_reason_codes.end());

  auto renewed_authority = authority("operator-session-2");
  renewed_authority.generation = 2;
  renewed_authority.valid_until_ns = kNow + (2 * kLeaseNs);
  ASSERT_TRUE(rig.coordinator->establish_authority_session(
    renewed_authority, kNow + kLeaseNs + 1));
  auto repeated = request_for(
    rig.coordinator->snapshot(kNow + kLeaseNs + 1),
    std::string(robot_lifecycle::kPrepareForMotionActionId), 1,
    "request-2");
  ASSERT_EQ(
    rig.coordinator->submit(
      repeated, kNow + kLeaseNs + 2).disposition,
    robot_lifecycle::CommandDisposition::kAccepted);

  ASSERT_TRUE(rig.coordinator->refresh(kNow + kLeaseNs + 3));
  ASSERT_TRUE(rig.coordinator->refresh(kNow + kLeaseNs + 4));
  const auto commands = rig.backend->command_log();
  ASSERT_EQ(commands.size(), prepared_commands.size() + 2U);
  EXPECT_EQ(
    commands[prepared_commands.size()],
    XArmLifecycleCommand::kDeactivateTrajectoryControllers);
  EXPECT_EQ(
    commands[prepared_commands.size() + 1],
    XArmLifecycleCommand::kActivateTrajectoryControllers);
  EXPECT_TRUE(
    rig.coordinator->motion_admitted(kNow + kLeaseNs + 4));
  const auto completed =
    rig.coordinator->snapshot(kNow + kLeaseNs + 4);
  ASSERT_TRUE(completed.last_terminal_transaction.has_value());
  EXPECT_EQ(
    completed.last_terminal_transaction->result_reason,
    "xarm_target_postcondition_observed");
}

TEST(
  EndToEndFakeTest,
  PrepareTargetReconcilesExternalManualModeWithoutCyclingDrivePower)
{
  auto rig = make_target_rig();
  establish_ready_authority(rig);
  auto first = request_for(
    rig.coordinator->snapshot(kNow),
    std::string(robot_lifecycle::kPrepareForMotionActionId));
  ASSERT_EQ(
    rig.coordinator->submit(first, kNow + 1).disposition,
    robot_lifecycle::CommandDisposition::kAccepted);
  for (std::int64_t offset = 2; offset <= 6; ++offset) {
    ASSERT_TRUE(rig.coordinator->refresh(kNow + offset));
  }
  ASSERT_TRUE(rig.coordinator->motion_admitted(kNow + 6));
  const auto prepared_command_count = rig.backend->command_log().size();

  rig.backend->set_vendor_mode_and_state(0, 2);
  ASSERT_TRUE(rig.coordinator->refresh(kNow + 7));
  EXPECT_FALSE(rig.coordinator->motion_admitted(kNow + 7));
  ASSERT_EQ(
    rig.coordinator->snapshot(kNow + 7).observed.robot_enabled,
    robot_lifecycle::FactState::kTrue);

  auto repeated = request_for(
    rig.coordinator->snapshot(kNow + 7),
    std::string(robot_lifecycle::kPrepareForMotionActionId), 2,
    "request-2");
  ASSERT_EQ(
    rig.coordinator->submit(repeated, kNow + 8).disposition,
    robot_lifecycle::CommandDisposition::kAccepted);
  for (std::int64_t offset = 9; offset <= 15; ++offset) {
    ASSERT_TRUE(rig.coordinator->refresh(kNow + offset));
    EXPECT_EQ(
      rig.coordinator->snapshot(kNow + offset).observed.robot_enabled,
      robot_lifecycle::FactState::kTrue);
  }

  const auto & commands = rig.backend->command_log();
  ASSERT_EQ(commands.size(), prepared_command_count + 7U);
  EXPECT_EQ(
    std::vector<XArmLifecycleCommand>(
      commands.begin() +
      static_cast<std::ptrdiff_t>(prepared_command_count),
      commands.end()),
    (std::vector<XArmLifecycleCommand>{
        XArmLifecycleCommand::kDeactivateTrajectoryControllers,
        XArmLifecycleCommand::kDeactivateHardwareComponent,
        XArmLifecycleCommand::kSetStoppedState,
        XArmLifecycleCommand::kSelectJointServoMode,
        XArmLifecycleCommand::kSetReadyState,
        XArmLifecycleCommand::kActivateHardwareComponent,
        XArmLifecycleCommand::kActivateTrajectoryControllers,
      }));
  EXPECT_TRUE(rig.coordinator->motion_admitted(kNow + 15));
  const auto completed = rig.coordinator->snapshot(kNow + 15);
  EXPECT_FALSE(completed.pending_transaction.has_value());
  ASSERT_TRUE(completed.last_terminal_transaction.has_value());
  EXPECT_EQ(
    completed.last_terminal_transaction->phase,
    robot_lifecycle::TransactionPhase::kSucceeded);
  EXPECT_EQ(
    completed.last_terminal_transaction->result_reason,
    "xarm_target_postcondition_observed");
  EXPECT_EQ(rig.backend->joint_write_count(), 0U);
}

TEST(
  EndToEndFakeTest,
  PrepareTargetAdoptsInitiallyEnabledManualModeWithoutCyclingDrivePower)
{
  auto rig = make_target_rig();
  rig.backend->set_motion_enabled(FactState::kTrue);
  rig.backend->set_vendor_mode_and_state(0, 2);
  rig.backend->set_hardware_component_active(true);
  rig.backend->set_trajectory_controller_active(true);
  establish_ready_authority(rig);

  const auto request = request_for(
    rig.coordinator->snapshot(kNow),
    std::string(robot_lifecycle::kPrepareForMotionActionId));
  ASSERT_EQ(
    rig.coordinator->submit(request, kNow + 1).disposition,
    robot_lifecycle::CommandDisposition::kAccepted);
  for (std::int64_t offset = 2; offset <= 8; ++offset) {
    ASSERT_TRUE(rig.coordinator->refresh(kNow + offset));
    EXPECT_EQ(
      rig.coordinator->snapshot(kNow + offset).observed.robot_enabled,
      robot_lifecycle::FactState::kTrue);
  }

  EXPECT_EQ(
    rig.backend->command_log(),
    (std::vector<XArmLifecycleCommand>{
        XArmLifecycleCommand::kDeactivateTrajectoryControllers,
        XArmLifecycleCommand::kDeactivateHardwareComponent,
        XArmLifecycleCommand::kSetStoppedState,
        XArmLifecycleCommand::kSelectJointServoMode,
        XArmLifecycleCommand::kSetReadyState,
        XArmLifecycleCommand::kActivateHardwareComponent,
        XArmLifecycleCommand::kActivateTrajectoryControllers,
      }));
  EXPECT_TRUE(rig.coordinator->motion_admitted(kNow + 8));
  EXPECT_EQ(rig.backend->joint_write_count(), 0U);
}

TEST(
  EndToEndFakeTest,
  PrepareTargetRejectsMovingManualModeWithoutPrimitive)
{
  auto rig = make_target_rig();
  rig.backend->set_motion_enabled(FactState::kTrue);
  rig.backend->set_vendor_mode_and_state(0, 2);
  rig.backend->set_hardware_component_active(true);
  rig.backend->set_trajectory_controller_active(true);
  rig.backend->set_stationary(FactState::kFalse);
  establish_ready_authority(rig);

  const auto request = request_for(
    rig.coordinator->snapshot(kNow),
    std::string(robot_lifecycle::kPrepareForMotionActionId));
  const auto rejected = rig.coordinator->submit(request, kNow + 1);

  EXPECT_EQ(
    rejected.disposition,
    robot_lifecycle::CommandDisposition::kRejected);
  EXPECT_EQ(rejected.reason, "xarm_target_precondition_not_satisfied");
  EXPECT_TRUE(rig.backend->command_log().empty());
  EXPECT_EQ(rig.backend->joint_write_count(), 0U);
}

TEST(
  EndToEndFakeTest,
  DisableTargetCompletesAfterStationaryEvidenceBecomesUnavailable)
{
  auto rig = make_target_rig();
  establish_ready_authority(rig);
  auto prepare = request_for(
    rig.coordinator->snapshot(kNow),
    std::string(robot_lifecycle::kPrepareForMotionActionId));
  ASSERT_EQ(
    rig.coordinator->submit(prepare, kNow + 1).disposition,
    robot_lifecycle::CommandDisposition::kAccepted);
  for (std::int64_t offset = 2; offset <= 6; ++offset) {
    ASSERT_TRUE(rig.coordinator->refresh(kNow + offset));
  }

  auto disable = request_for(
    rig.coordinator->snapshot(kNow + 6),
    std::string(robot_lifecycle::kMakeNoncommandableActionId), 2,
    "request-2");
  ASSERT_EQ(
    rig.coordinator->submit(disable, kNow + 7).disposition,
    robot_lifecycle::CommandDisposition::kAccepted);
  ASSERT_TRUE(rig.coordinator->refresh(kNow + 8));
  ASSERT_TRUE(rig.coordinator->refresh(kNow + 9));
  ASSERT_TRUE(rig.coordinator->refresh(kNow + 10));
  rig.backend->set_stationary(FactState::kUnknown);
  ASSERT_TRUE(rig.coordinator->refresh(kNow + 11));

  const auto snapshot = rig.coordinator->snapshot(kNow + 11);
  EXPECT_FALSE(snapshot.pending_transaction.has_value());
  ASSERT_TRUE(snapshot.last_terminal_transaction.has_value());
  EXPECT_EQ(
    snapshot.last_terminal_transaction->phase,
    robot_lifecycle::TransactionPhase::kSucceeded);
  EXPECT_EQ(
    snapshot.observed.stationary,
    robot_lifecycle::FactState::kUnknown);
  EXPECT_EQ(
    snapshot.observed.robot_enabled,
    robot_lifecycle::FactState::kFalse);
  const auto & commands = rig.backend->command_log();
  ASSERT_GE(commands.size(), 9U);
  EXPECT_EQ(
    std::vector<XArmLifecycleCommand>(commands.end() - 4, commands.end()),
    (std::vector<XArmLifecycleCommand>{
        XArmLifecycleCommand::kDeactivateTrajectoryControllers,
        XArmLifecycleCommand::kDeactivateHardwareComponent,
        XArmLifecycleCommand::kSetStoppedState,
        XArmLifecycleCommand::kDisableMotion,
      }));
}

TEST(
  EndToEndFakeTest,
  DisableTargetRecoversStationaryManualPositionControlState)
{
  auto rig = make_target_rig();
  rig.backend->set_motion_enabled(FactState::kTrue);
  rig.backend->set_vendor_mode_and_state(0, 2);
  rig.backend->set_auto_apply_commands(false);
  establish_ready_authority(rig);
  const auto observed = rig.coordinator->snapshot(kNow);
  ASSERT_EQ(
    observed.observed.robot_enabled,
    robot_lifecycle::FactState::kTrue);
  ASSERT_EQ(
    observed.observed.vendor_control_ready,
    robot_lifecycle::FactState::kFalse);

  const auto disable = request_for(
    observed,
    std::string(robot_lifecycle::kMakeNoncommandableActionId));
  ASSERT_EQ(
    rig.coordinator->submit(disable, kNow + 1).disposition,
    robot_lifecycle::CommandDisposition::kAccepted);
  ASSERT_TRUE(rig.coordinator->refresh(kNow + 2));

  EXPECT_EQ(
    rig.backend->command_log(),
    (std::vector<XArmLifecycleCommand>{
        XArmLifecycleCommand::kSetStoppedState,
      }));
  EXPECT_TRUE(
    rig.coordinator->snapshot(kNow + 2).pending_transaction.has_value());

  rig.backend->apply_pending_effects();
  ASSERT_TRUE(rig.coordinator->refresh(kNow + 3));
  EXPECT_EQ(
    rig.backend->command_log(),
    (std::vector<XArmLifecycleCommand>{
        XArmLifecycleCommand::kSetStoppedState,
        XArmLifecycleCommand::kDisableMotion,
      }));
  const auto stopped = rig.coordinator->snapshot(kNow + 3);
  EXPECT_TRUE(stopped.pending_transaction.has_value());
  EXPECT_EQ(
    stopped.observed.robot_enabled,
    robot_lifecycle::FactState::kTrue);

  ASSERT_TRUE(rig.coordinator->refresh(kNow + 4));
  EXPECT_EQ(rig.backend->command_log().size(), 2U);
  rig.backend->apply_pending_effects();
  ASSERT_TRUE(rig.coordinator->refresh(kNow + 5));

  const auto completed = rig.coordinator->snapshot(kNow + 5);
  EXPECT_FALSE(completed.pending_transaction.has_value());
  ASSERT_TRUE(completed.last_terminal_transaction.has_value());
  EXPECT_EQ(
    completed.last_terminal_transaction->phase,
    robot_lifecycle::TransactionPhase::kSucceeded);
  EXPECT_EQ(
    completed.observed.robot_enabled,
    robot_lifecycle::FactState::kFalse);
  EXPECT_EQ(
    completed.observed.vendor_control_ready,
    robot_lifecycle::FactState::kFalse);
}

TEST(
  EndToEndFakeTest,
  TargetWaitsForStagePostconditionBeforeSelectingAnotherStage)
{
  auto rig = make_target_rig();
  rig.backend->set_auto_apply_commands(false);
  establish_ready_authority(rig);
  const auto request = request_for(
    rig.coordinator->snapshot(kNow),
    std::string(robot_lifecycle::kPrepareForMotionActionId));

  ASSERT_EQ(
    rig.coordinator->submit(request, kNow + 1).disposition,
    robot_lifecycle::CommandDisposition::kAccepted);
  ASSERT_TRUE(rig.coordinator->refresh(kNow + 2));
  EXPECT_EQ(rig.backend->command_log().size(), 1U);

  rig.backend->apply_pending_effects();
  ASSERT_TRUE(rig.coordinator->refresh(kNow + 3));
  EXPECT_EQ(rig.backend->command_log().size(), 2U);
}

TEST(
  EndToEndFakeTest,
  AmbiguousTargetStageRequiresReconciliationAndIsNeverRepeated)
{
  auto rig = make_target_rig();
  rig.backend->set_next_return_code(-5);
  establish_ready_authority(rig);
  const auto request = request_for(
    rig.coordinator->snapshot(kNow),
    std::string(robot_lifecycle::kPrepareForMotionActionId));

  const auto accepted = rig.coordinator->submit(request, kNow + 1);

  ASSERT_EQ(
    accepted.disposition,
    robot_lifecycle::CommandDisposition::kAccepted);
  const auto snapshot = rig.coordinator->snapshot(kNow + 1);
  ASSERT_TRUE(snapshot.pending_transaction.has_value());
  EXPECT_EQ(
    snapshot.pending_transaction->phase,
    robot_lifecycle::TransactionPhase::kOutcomeUnknown);
  EXPECT_TRUE(snapshot.reconciliation_required);
  EXPECT_EQ(rig.backend->command_log().size(), 1U);
  ASSERT_TRUE(rig.coordinator->refresh(kNow + 2));
  EXPECT_EQ(rig.backend->command_log().size(), 1U);
}

TEST(
  EndToEndFakeTest,
  AuthorityExpiryMidTargetFencesProgressAndNeverRepeatsAStage)
{
  auto rig = make_target_rig();
  rig.backend->set_auto_apply_commands(false);
  establish_ready_authority(rig);
  const auto request = request_for(
    rig.coordinator->snapshot(kNow),
    std::string(robot_lifecycle::kPrepareForMotionActionId));

  ASSERT_EQ(
    rig.coordinator->submit(request, kNow + 1).disposition,
    robot_lifecycle::CommandDisposition::kAccepted);
  ASSERT_EQ(rig.backend->command_log().size(), 1U);

  ASSERT_TRUE(rig.coordinator->refresh(kNow + kLeaseNs));
  const auto expired =
    rig.coordinator->snapshot(kNow + kLeaseNs);
  ASSERT_TRUE(expired.pending_transaction.has_value());
  EXPECT_EQ(
    expired.pending_transaction->phase,
    robot_lifecycle::TransactionPhase::kOutcomeUnknown);
  EXPECT_EQ(
    expired.pending_transaction->result_reason,
    "authority_session_stale");
  EXPECT_TRUE(expired.reconciliation_required);
  EXPECT_FALSE(expired.motion_admitted);
  EXPECT_EQ(rig.backend->command_log().size(), 1U);

  rig.backend->apply_pending_effects();
  ASSERT_TRUE(rig.coordinator->refresh(kNow + kLeaseNs + 1));
  EXPECT_EQ(rig.backend->command_log().size(), 1U);
}

TEST(EndToEndFakeTest, FreshnessPollingDoesNotInvalidateConfirmedSnapshot)
{
  auto rig = make_rig();
  establish_ready_authority(rig);
  const auto confirmed = rig.coordinator->snapshot(kNow);
  const auto request = request_for(confirmed);

  for (std::int64_t offset = 1; offset <= 100; ++offset) {
    ASSERT_TRUE(rig.coordinator->refresh(kNow + offset));
  }
  EXPECT_EQ(
    rig.coordinator->snapshot(kNow + 100).generation,
    confirmed.generation);
  EXPECT_EQ(
    rig.coordinator->submit(request, kNow + 101).disposition,
    robot_lifecycle::CommandDisposition::kAccepted);
}

TEST(EndToEndFakeTest, RejectionNeverReachesTheHardwareOwner)
{
  auto rig = make_rig();
  ASSERT_TRUE(rig.coordinator->refresh(kNow));
  robot_lifecycle::CommandRequest request;
  request.robot_id = "lite6-a";
  request.request_id = "request-without-authority";
  request.request_sequence = 1;
  request.action_id = "robot.enable";
  request.authority_profile =
    robot_lifecycle::AuthorityProfile::kLocalOperator;
  request.expected_snapshot_generation =
    rig.coordinator->snapshot(kNow).generation;
  request.confirmation_evidence = "test-confirmation";

  const auto rejected = rig.coordinator->submit(request, kNow + 1);

  EXPECT_EQ(
    rejected.disposition,
    robot_lifecycle::CommandDisposition::kRejected);
  EXPECT_EQ(rejected.reason, "authority_session_missing");
  EXPECT_TRUE(rig.backend->command_log().empty());
  EXPECT_TRUE(rig.coordinator->primitive_audit_snapshot().empty());
}

TEST(EndToEndFakeTest, WaitsForDelayedObservedPostcondition)
{
  auto rig = make_rig();
  rig.backend->set_auto_apply_commands(false);
  establish_ready_authority(rig);
  const auto request = request_for(rig.coordinator->snapshot(kNow));

  const auto accepted = rig.coordinator->submit(request, kNow + 1);

  ASSERT_EQ(
    accepted.disposition,
    robot_lifecycle::CommandDisposition::kAccepted);
  auto snapshot = rig.coordinator->snapshot(kNow + 1);
  ASSERT_TRUE(snapshot.pending_transaction.has_value());
  EXPECT_EQ(
    snapshot.pending_transaction->phase,
    robot_lifecycle::TransactionPhase::kVerifying);

  rig.backend->apply_pending_effects();
  ASSERT_TRUE(rig.coordinator->refresh(kNow + 2));
  snapshot = rig.coordinator->snapshot(kNow + 2);
  EXPECT_FALSE(snapshot.pending_transaction.has_value());
  ASSERT_TRUE(snapshot.last_terminal_transaction.has_value());
  EXPECT_EQ(
    snapshot.last_terminal_transaction->phase,
    robot_lifecycle::TransactionPhase::kSucceeded);
}

TEST(EndToEndFakeTest, AmbiguousPrimitiveRequiresExplicitReconciliation)
{
  auto rig = make_rig();
  rig.backend->set_next_return_code(-5);
  establish_ready_authority(rig);
  const auto request = request_for(rig.coordinator->snapshot(kNow));

  const auto accepted = rig.coordinator->submit(request, kNow + 1);

  ASSERT_EQ(
    accepted.disposition,
    robot_lifecycle::CommandDisposition::kAccepted);
  ASSERT_EQ(rig.backend->command_log().size(), 1U);
  auto snapshot = rig.coordinator->snapshot(kNow + 1);
  ASSERT_TRUE(snapshot.pending_transaction.has_value());
  EXPECT_EQ(
    snapshot.pending_transaction->phase,
    robot_lifecycle::TransactionPhase::kOutcomeUnknown);
  EXPECT_TRUE(snapshot.reconciliation_required);
  const auto audit = rig.coordinator->primitive_audit_snapshot();
  ASSERT_EQ(audit.size(), 1U);
  EXPECT_EQ(audit.front().result.return_code, -5);
  EXPECT_EQ(
    audit.front().result.reason,
    "fake_command_result_ambiguous");

  rig.backend->set_motion_enabled(robot_lifecycle::FactState::kTrue);
  rig.backend->set_vendor_control_ready(
    robot_lifecycle::FactState::kTrue);
  ASSERT_TRUE(rig.coordinator->refresh(kNow + 2));
  snapshot = rig.coordinator->snapshot(kNow + 2);
  ASSERT_TRUE(snapshot.pending_transaction.has_value());
  EXPECT_EQ(
    snapshot.pending_transaction->phase,
    robot_lifecycle::TransactionPhase::kOutcomeUnknown);
  EXPECT_EQ(rig.backend->command_log().size(), 1U);

  ASSERT_TRUE(rig.coordinator->reconcile(
    accepted.transaction_generation, request.request_id,
    "operator_observed_postcondition", kNow + 3));
  snapshot = rig.coordinator->snapshot(kNow + 3);
  EXPECT_FALSE(snapshot.reconciliation_required);
  ASSERT_TRUE(snapshot.last_terminal_transaction.has_value());
  EXPECT_EQ(
    snapshot.last_terminal_transaction->phase,
    robot_lifecycle::TransactionPhase::kSucceeded);
  EXPECT_EQ(rig.backend->command_log().size(), 1U);
}

TEST(EndToEndFakeTest, PartialMultiStepSequenceIsNeverReportedAsKnownFailure)
{
  XArmActionPlan partial_plan;
  partial_plan.action_id = "robot.test_partial";
  partial_plan.label = "Exercise partial sequence fencing";
  partial_plan.confirmation =
    robot_lifecycle::ConfirmationClass::kExplicit;
  partial_plan.side_effect_summary = "Test-only two-step transition.";
  partial_plan.expected_postcondition =
    "The fake backend reports motion enabled.";
  partial_plan.timeout_ns = kLeaseNs;
  partial_plan.preconditions = {
    {XArmConditionKind::kTransportConnected, 1},
    {XArmConditionKind::kIdentityVerified, 1},
  };
  partial_plan.steps = {
    XArmLifecycleCommand::kClearWarning,
    static_cast<XArmLifecycleCommand>(255),
  };
  partial_plan.postconditions = {
    {XArmConditionKind::kMotionEnabled, 1}};
  std::vector<XArmActionPlan> plans;
  plans.push_back(std::move(partial_plan));
  auto rig = make_rig(
    "lite6-a", "transport-owner-a", std::move(plans));
  establish_ready_authority(rig);
  const auto request = request_for(
    rig.coordinator->snapshot(kNow), "robot.test_partial");

  const auto accepted = rig.coordinator->submit(request, kNow + 1);

  ASSERT_EQ(
    accepted.disposition,
    robot_lifecycle::CommandDisposition::kAccepted);
  ASSERT_EQ(rig.backend->command_log().size(), 1U);
  EXPECT_EQ(
    rig.backend->command_log().front(),
    XArmLifecycleCommand::kClearWarning);
  const auto snapshot = rig.coordinator->snapshot(kNow + 1);
  ASSERT_TRUE(snapshot.pending_transaction.has_value());
  EXPECT_EQ(
    snapshot.pending_transaction->phase,
    robot_lifecycle::TransactionPhase::kOutcomeUnknown);
  EXPECT_EQ(
    snapshot.pending_transaction->result_reason,
    "xarm_partial_sequence_result_ambiguous");
}

TEST(EndToEndFakeTest, PrimitiveAuditIsBoundedWithoutLosingSequenceIdentity)
{
  auto rig = make_rig(
    "lite6-a", "transport-owner-a",
    make_fake_xarm_action_plans(), 1);
  establish_ready_authority(rig);
  const auto first = request_for(rig.coordinator->snapshot(kNow));
  ASSERT_EQ(
    rig.coordinator->submit(first, kNow + 1).disposition,
    robot_lifecycle::CommandDisposition::kAccepted);

  auto second = request_for(
    rig.coordinator->snapshot(kNow + 1), "robot.disable", 2,
    "request-2");
  ASSERT_EQ(
    rig.coordinator->submit(second, kNow + 2).disposition,
    robot_lifecycle::CommandDisposition::kAccepted);

  const auto audit = rig.coordinator->primitive_audit_snapshot();
  ASSERT_EQ(audit.size(), 1U);
  EXPECT_EQ(audit.front().sequence, 2U);
  EXPECT_EQ(audit.front().action_id, "robot.disable");
  EXPECT_EQ(
    audit.front().command,
    XArmLifecycleCommand::kDisableMotion);
}

TEST(
  EndToEndFakeTest,
  ControllerShutdownTerminatesOnKnownVendorAcceptanceAndRestartFence)
{
  auto rig = make_rig(
    "lite6-a", "transport-owner-a",
    make_lite6_xarm_action_plans());
  rig.backend->set_controller_shutdown_eligible(FactState::kTrue);
  establish_ready_authority(rig);
  const auto request = request_for(
    rig.coordinator->snapshot(kNow), "shutdown_controller");

  const auto accepted = rig.coordinator->submit(request, kNow + 1);

  ASSERT_EQ(
    accepted.disposition,
    robot_lifecycle::CommandDisposition::kAccepted);
  ASSERT_EQ(rig.backend->command_log().size(), 1U);
  EXPECT_EQ(
    rig.backend->command_log().front(),
    XArmLifecycleCommand::kShutdownController);
  const auto completed = rig.coordinator->snapshot(kNow + 1);
  EXPECT_FALSE(completed.pending_transaction.has_value());
  ASSERT_TRUE(completed.last_terminal_transaction.has_value());
  EXPECT_EQ(
    completed.last_terminal_transaction->phase,
    robot_lifecycle::TransactionPhase::kSucceeded);
  EXPECT_EQ(
    completed.last_terminal_transaction->result_reason,
    "controller_shutdown_vendor_accepted_process_restart_required");
  const auto audit = rig.coordinator->primitive_audit_snapshot();
  ASSERT_EQ(audit.size(), 1U);
  EXPECT_TRUE(audit.front().result.permitted);
  EXPECT_TRUE(audit.front().result.attempted);
  EXPECT_EQ(audit.front().result.return_code, 0);
  EXPECT_TRUE(audit.front().result.process_restart_required);
  EXPECT_FALSE(rig.coordinator->motion_admitted(kNow + 1));
}

TEST(EndToEndFakeTest, JointWritesRemainClosedUntilEveryGateIsTrue)
{
  auto rig = make_rig();
  establish_ready_authority(rig);
  rig.coordinator->set_hardware_component_active(true);
  ASSERT_TRUE(rig.coordinator->refresh(kNow + 1));
  const std::vector<double> target(kJointCount, 0.25);
  const std::vector<double> velocity(kJointCount, 0.05);
  const JointCommandView command{target, velocity};

  EXPECT_FALSE(rig.coordinator->write_joint_command(command, kNow + 1));
  EXPECT_EQ(rig.backend->joint_write_count(), 0U);

  const auto request = request_for(
    rig.coordinator->snapshot(kNow + 1));
  ASSERT_EQ(
    rig.coordinator->submit(request, kNow + 2).disposition,
    robot_lifecycle::CommandDisposition::kAccepted);
  EXPECT_FALSE(rig.coordinator->motion_admitted(kNow + 2));
  rig.coordinator->set_trajectory_controller_active(true);
  ASSERT_TRUE(rig.coordinator->refresh(kNow + 3));
  EXPECT_TRUE(rig.coordinator->motion_admitted(kNow + 3));
  EXPECT_TRUE(rig.coordinator->write_joint_command(command, kNow + 3));
  EXPECT_EQ(rig.backend->joint_write_count(), 1U);
  EXPECT_EQ(rig.backend->last_joint_write(), target);
  EXPECT_EQ(rig.backend->last_joint_velocity_write(), velocity);

  rig.coordinator->set_trajectory_controller_active(false);
  EXPECT_FALSE(rig.coordinator->motion_admitted(kNow + 3));
  EXPECT_FALSE(rig.coordinator->write_joint_command(command, kNow + 3));
  EXPECT_EQ(rig.backend->joint_write_count(), 1U);
}

TEST(EndToEndFakeTest, AdmissionLeaseExpiresWithoutTouchingTheReducer)
{
  auto rig = make_rig();
  establish_ready_authority(rig);
  rig.coordinator->set_hardware_component_active(true);
  ASSERT_TRUE(rig.coordinator->refresh(kNow + 1));
  const auto request = request_for(
    rig.coordinator->snapshot(kNow + 1));
  ASSERT_EQ(
    rig.coordinator->submit(request, kNow + 2).disposition,
    robot_lifecycle::CommandDisposition::kAccepted);
  rig.coordinator->set_trajectory_controller_active(true);
  ASSERT_TRUE(rig.coordinator->refresh(kNow + 3));

  EXPECT_TRUE(rig.coordinator->motion_admitted(kNow + 3));
  EXPECT_FALSE(rig.coordinator->motion_admitted(kNow + kLeaseNs));
}

TEST(EndToEndFakeTest, AdmissionCannotReopenWithoutControllerRearm)
{
  auto rig = make_rig();
  establish_ready_authority(rig);
  rig.coordinator->set_hardware_component_active(true);
  ASSERT_TRUE(rig.coordinator->refresh(kNow + 1));
  const auto request = request_for(
    rig.coordinator->snapshot(kNow + 1));
  ASSERT_EQ(
    rig.coordinator->submit(request, kNow + 2).disposition,
    robot_lifecycle::CommandDisposition::kAccepted);
  rig.coordinator->set_trajectory_controller_active(true);
  ASSERT_TRUE(rig.coordinator->refresh(kNow + 3));
  ASSERT_TRUE(rig.coordinator->motion_admitted(kNow + 3));

  ASSERT_TRUE(rig.coordinator->clear_authority_session(
    "operator-session", 1, kNow + 4));
  auto replacement = authority("replacement-session");
  replacement.generation = 2;
  ASSERT_TRUE(rig.coordinator->establish_authority_session(
    replacement, kNow + 5));
  ASSERT_TRUE(rig.coordinator->refresh(kNow + 6));
  auto blocked = rig.coordinator->snapshot(kNow + 6);
  EXPECT_FALSE(blocked.motion_admitted);
  EXPECT_NE(
    std::find(
      blocked.admission_reason_codes.begin(),
      blocked.admission_reason_codes.end(),
      "trajectory_controller_rearm_required"),
    blocked.admission_reason_codes.end());

  rig.coordinator->set_trajectory_controller_active(false);
  ASSERT_TRUE(rig.coordinator->refresh(kNow + 7));
  rig.coordinator->set_trajectory_controller_active(true);
  ASSERT_TRUE(rig.coordinator->refresh(kNow + 8));
  EXPECT_TRUE(rig.coordinator->motion_admitted(kNow + 8));
}

TEST(EndToEndFakeTest, TwoRobotInstancesHaveIndependentSoleOwners)
{
  auto first = make_rig("lite6-a", "transport-owner-a");
  auto second = make_rig("lite6-b", "transport-owner-b");
  establish_ready_authority(first);
  establish_ready_authority(second);

  const auto first_request = request_for(
    first.coordinator->snapshot(kNow), "robot.enable", 1,
    "request-a");
  ASSERT_EQ(
    first.coordinator->submit(first_request, kNow + 1).disposition,
    robot_lifecycle::CommandDisposition::kAccepted);

  EXPECT_EQ(first.coordinator->owner_id(), "transport-owner-a");
  EXPECT_EQ(second.coordinator->owner_id(), "transport-owner-b");
  EXPECT_EQ(first.backend->command_log().size(), 1U);
  EXPECT_TRUE(second.backend->command_log().empty());
  EXPECT_EQ(
    second.coordinator->snapshot(kNow + 1).observed.robot_enabled,
    robot_lifecycle::FactState::kFalse);
}

TEST(FakeBackendConcurrencyTest, SerializesNrtObservationAndRtJointAccess)
{
  FakeXArmHardwareBackend backend(
    "concurrency-test-owner", kJointCount, kLeaseNs);
  ASSERT_EQ(
    backend.set_command_gate(true, kNow + kLeaseNs).state,
    XArmCommandGateUpdateState::kApplied);
  std::atomic<bool> begin{false};
  std::atomic<bool> rt_access_succeeded{true};

  std::thread rt_thread(
    [&backend, &begin, &rt_access_succeeded]()
    {
      while (!begin.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      JointStateSample sample;
      for (std::size_t iteration = 0;
      iteration < kConcurrentRtIterations; ++iteration)
      {
        if (!backend.read_joint_state(sample) ||
        sample.positions.size() != kJointCount)
        {
          rt_access_succeeded.store(false, std::memory_order_release);
          return;
        }
        sample.positions[0] =
        static_cast<double>(iteration % 100) / 100.0;
        const JointCommandView command{
          sample.positions, sample.velocities};
        if (!backend.write_joint_command(command)) {
          rt_access_succeeded.store(false, std::memory_order_release);
          return;
        }
      }
    });

  std::thread nrt_thread(
    [&backend, &begin]()
    {
      begin.store(true, std::memory_order_release);
      for (std::int64_t iteration = 0;
      iteration < kConcurrentNrtIterations; ++iteration)
      {
        const bool active = iteration % 2 == 0;
        backend.set_hardware_component_active(active);
        backend.set_trajectory_controller_active(active);
        backend.set_motion_enabled(
          active ? robot_lifecycle::FactState::kTrue :
          robot_lifecycle::FactState::kFalse);
        const auto observation = backend.observe(kNow + iteration);
        if (observation.generation == 0) {
          return;
        }
      }
    });

  rt_thread.join();
  nrt_thread.join();
  EXPECT_TRUE(rt_access_succeeded.load(std::memory_order_acquire));
  EXPECT_EQ(backend.joint_write_count(), kConcurrentRtIterations);
}

}  // namespace
}  // namespace manyforge::xarm_control
