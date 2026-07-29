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
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <manyforge_robot_lifecycle/supervisor.hpp>

namespace manyforge::robot_lifecycle
{
namespace
{

constexpr std::int64_t kNow = 1'000'000;
constexpr std::int64_t kLease = 100'000;

Supervisor make_supervisor(
  AuthorityProfile profile = AuthorityProfile::kLocalOperator,
  std::size_t replay_capacity = 128,
  std::string robot_id = "robot-1")
{
  return Supervisor(
      SupervisorConfig{
        std::move(robot_id), "supervisor-instance-1", profile,
        replay_capacity});
}

ObservedFacets ready_observation(std::uint64_t generation = 1)
{
  ObservedFacets observation;
  observation.generation = generation;
  observation.source_timestamp_ns = kNow - 10;
  observation.received_timestamp_ns = kNow;
  observation.fresh_until_ns = kNow + kLease;
  observation.transport_connected = FactState::kTrue;
  observation.identity_verified = FactState::kTrue;
  observation.position_valid = FactState::kTrue;
  observation.robot_enabled = FactState::kTrue;
  observation.vendor_control_ready = FactState::kTrue;
  observation.hardware_component_active = FactState::kTrue;
  observation.trajectory_controller_active = FactState::kTrue;
  observation.fault_present = FactState::kFalse;
  observation.recovery_required = FactState::kFalse;
  return observation;
}

CapabilityDescriptor capability(
  std::string action_id = "robot.prepare",
  ConfirmationClass confirmation = ConfirmationClass::kExplicit)
{
  return CapabilityDescriptor{
    std::move(action_id),
    "Prepare robot",
    true,
    true,
    "",
    confirmation,
    "May enable robot control after adapter-specific checks.",
    "Adapter reports the robot and control path ready.",
    kLease};
}

AuthoritySession authority_session(
  AuthorityProfile profile = AuthorityProfile::kLocalOperator,
  std::uint64_t generation = 1,
  std::string session_id = "operator-session-1")
{
  return AuthoritySession{
    profile,
    std::move(session_id),
    generation,
    "human:operator",
    kNow + kLease,
    "operator-epoch-1"};
}

CommandRequest request_for(
  const LifecycleSnapshot & snapshot,
  std::uint64_t sequence = 1,
  std::string request_id = "request-1",
  std::string action_id = "robot.prepare")
{
  return CommandRequest{
    snapshot.robot_id,
    std::move(request_id),
    sequence,
    std::move(action_id),
    snapshot.authority_profile,
    snapshot.authority_session->session_id,
    snapshot.authority_session->generation,
    snapshot.authority_session->principal_id,
    snapshot.generation,
    "confirmed-by-operator"};
}

bool has_reason(
  const LifecycleSnapshot & snapshot, const std::string & reason)
{
  return std::find(
             snapshot.admission_reason_codes.begin(),
             snapshot.admission_reason_codes.end(),
             reason) != snapshot.admission_reason_codes.end();
}

TEST(SupervisorTest, StartsFailClosedWithoutInventingUnknownFacts) {
  auto supervisor = make_supervisor();
  const auto snapshot = supervisor.snapshot(kNow);

  EXPECT_FALSE(snapshot.observation_fresh);
  EXPECT_FALSE(snapshot.authority_fresh);
  EXPECT_FALSE(snapshot.observe_ready);
  EXPECT_FALSE(snapshot.controller_ready);
  EXPECT_FALSE(snapshot.motion_admitted);
  EXPECT_TRUE(has_reason(snapshot, "observation_stale_or_missing"));
  EXPECT_TRUE(has_reason(snapshot, "identity_not_verified"));
  EXPECT_TRUE(has_reason(snapshot, "fault_present_or_unknown"));
  EXPECT_TRUE(has_reason(snapshot, "authority_session_stale_or_missing"));
}

TEST(SupervisorTest, DerivesReadinessFromOrthogonalFreshFacts) {
  auto supervisor = make_supervisor();
  ASSERT_TRUE(supervisor.publish_observation(ready_observation()));
  ASSERT_TRUE(supervisor.replace_capabilities(1, 1, {capability()}));
  ASSERT_TRUE(
      supervisor.establish_authority_session(authority_session(), kNow));

  auto snapshot = supervisor.snapshot(kNow);
  EXPECT_TRUE(snapshot.observation_fresh);
  EXPECT_TRUE(snapshot.authority_fresh);
  EXPECT_TRUE(snapshot.observe_ready);
  EXPECT_TRUE(snapshot.controller_ready);
  EXPECT_TRUE(snapshot.motion_admitted);
  EXPECT_TRUE(supervisor.motion_admitted(kNow));
  EXPECT_TRUE(snapshot.admission_reason_codes.empty());

  snapshot = supervisor.snapshot(kNow + kLease);
  EXPECT_FALSE(snapshot.observation_fresh);
  EXPECT_FALSE(snapshot.authority_fresh);
  EXPECT_FALSE(snapshot.motion_admitted);
  EXPECT_FALSE(supervisor.motion_admitted(kNow + kLease));
}

TEST(SupervisorTest, ContinuousLeaseRenewalPreservesSemanticGeneration) {
  auto supervisor = make_supervisor();
  ASSERT_TRUE(supervisor.publish_observation(ready_observation()));
  ASSERT_TRUE(supervisor.replace_capabilities(1, 1, {capability()}));
  const auto before = supervisor.snapshot(kNow);

  ASSERT_TRUE(supervisor.renew_observation_lease(
    1, kNow + 10, kNow + 20, kNow + kLease + 20));
  const auto renewed = supervisor.snapshot(kNow + 20);

  EXPECT_EQ(renewed.generation, before.generation);
  EXPECT_EQ(renewed.observed.generation, before.observed.generation);
  EXPECT_EQ(renewed.capability_generation, before.capability_generation);
  EXPECT_EQ(renewed.observed.received_timestamp_ns, kNow + 20);
  EXPECT_TRUE(renewed.observation_fresh);

  EXPECT_FALSE(supervisor.renew_observation_lease(
    1, kNow + kLease + 20, kNow + kLease + 20,
    kNow + (2 * kLease)));
}

TEST(
  SupervisorTest,
  RenewsSameAuthorityGenerationWithoutResettingReplayDomain)
{
  auto supervisor = make_supervisor();
  ASSERT_TRUE(supervisor.publish_observation(ready_observation()));
  ASSERT_TRUE(supervisor.replace_capabilities(1, 1, {capability()}));
  auto session = authority_session();
  ASSERT_TRUE(supervisor.establish_authority_session(session, kNow));

  const auto request = request_for(supervisor.snapshot(kNow));
  const auto decision = supervisor.submit(request, kNow);
  ASSERT_EQ(decision.disposition, CommandDisposition::kAccepted);
  ASSERT_TRUE(supervisor.mark_executing(
      decision.transaction_generation, request.request_id, kNow + 1));
  ASSERT_TRUE(supervisor.mark_verifying(
      decision.transaction_generation, request.request_id, kNow + 2));
  ASSERT_TRUE(supervisor.finish_success(
      decision.transaction_generation, request.request_id,
      "target_observed", kNow + 3));

  session.valid_until_ns = kNow + 2 * kLease;
  ASSERT_TRUE(
      supervisor.establish_authority_session(session, kNow + 4));
  const auto renewed = supervisor.snapshot(kNow + 4);
  ASSERT_TRUE(renewed.authority_session.has_value());
  EXPECT_EQ(renewed.authority_session->generation, 1U);
  EXPECT_EQ(
    renewed.authority_session->valid_until_ns,
    kNow + 2 * kLease);

  const auto regressed = request_for(
    renewed, 1, "request-with-reused-sequence");
  const auto rejected = supervisor.submit(regressed, kNow + 5);
  EXPECT_EQ(rejected.disposition, CommandDisposition::kRejected);
  EXPECT_EQ(rejected.reason, "request_sequence_conflict");

  const auto replay = supervisor.submit(request, kNow + 6);
  EXPECT_EQ(replay.disposition, CommandDisposition::kIdempotentReplay);
  EXPECT_EQ(replay.phase, TransactionPhase::kSucceeded);
}

TEST(SupervisorTest, ExpiredAuthorityGenerationCannotBeRenewed) {
  auto supervisor = make_supervisor();
  auto session = authority_session();
  ASSERT_TRUE(supervisor.establish_authority_session(session, kNow));

  session.valid_until_ns = kNow + 2 * kLease;
  EXPECT_FALSE(
      supervisor.establish_authority_session(session, kNow + kLease));
  EXPECT_FALSE(supervisor.snapshot(kNow + kLease).authority_fresh);

  auto replacement = session;
  replacement.generation = 2;
  replacement.session_id = "operator-session-2";
  EXPECT_TRUE(
      supervisor.establish_authority_session(replacement, kNow + kLease));
  const auto snapshot = supervisor.snapshot(kNow + kLease);
  ASSERT_TRUE(snapshot.authority_session.has_value());
  EXPECT_EQ(snapshot.authority_session->generation, 2U);
  EXPECT_TRUE(snapshot.authority_fresh);
}

TEST(SupervisorTest, AcceptsOnlyCurrentSelectedAuthorityAndSnapshot) {
  auto supervisor = make_supervisor();
  ASSERT_TRUE(supervisor.publish_observation(ready_observation()));
  ASSERT_TRUE(supervisor.replace_capabilities(1, 1, {capability()}));
  ASSERT_TRUE(
      supervisor.establish_authority_session(authority_session(), kNow));

  auto request = request_for(supervisor.snapshot(kNow));
  request.authority_profile = AuthorityProfile::kExternalPlc;
  EXPECT_EQ(
      supervisor.submit(request, kNow).reason,
      "authority_profile_mismatch");

  request = request_for(supervisor.snapshot(kNow));
  request.expected_snapshot_generation -= 1;
  EXPECT_EQ(
      supervisor.submit(request, kNow).reason,
      "snapshot_generation_mismatch");

  request = request_for(supervisor.snapshot(kNow), 2, "request-2");
  const auto accepted = supervisor.submit(request, kNow);
  EXPECT_EQ(accepted.disposition, CommandDisposition::kAccepted);
  EXPECT_NE(accepted.transaction_generation, 0u);
  EXPECT_FALSE(supervisor.snapshot(kNow).motion_admitted);
}

TEST(SupervisorTest, RequiresOrderedRequestsAndReplaysExactlyOnce) {
  auto supervisor = make_supervisor(
      AuthorityProfile::kLocalOperator, 1);
  ASSERT_TRUE(supervisor.publish_observation(ready_observation()));
  ASSERT_TRUE(supervisor.replace_capabilities(1, 1, {capability()}));
  ASSERT_TRUE(
      supervisor.establish_authority_session(authority_session(), kNow));

  auto gap = request_for(supervisor.snapshot(kNow), 2, "request-2");
  EXPECT_EQ(supervisor.submit(gap, kNow).reason, "request_sequence_gap");

  auto first = request_for(supervisor.snapshot(kNow));
  const auto accepted = supervisor.submit(first, kNow);
  ASSERT_EQ(accepted.disposition, CommandDisposition::kAccepted);
  const auto replay = supervisor.submit(first, kNow + 1);
  EXPECT_EQ(replay.disposition, CommandDisposition::kIdempotentReplay);
  EXPECT_EQ(replay.transaction_generation, accepted.transaction_generation);

  auto conflict = first;
  conflict.action_id = "robot.disconnect";
  EXPECT_EQ(
      supervisor.submit(conflict, kNow + 1).reason,
      "request_sequence_conflict");

  ASSERT_TRUE(supervisor.finish_failure(
      accepted.transaction_generation, first.request_id,
      "fake_adapter_rejected", kNow + 2));
  auto second =
    request_for(supervisor.snapshot(kNow + 2), 2, "request-2");
  EXPECT_EQ(
      supervisor.submit(second, kNow + 2).disposition,
      CommandDisposition::kAccepted);

  EXPECT_EQ(
      supervisor.submit(first, kNow + 3).reason,
      "request_sequence_retired");
}

TEST(SupervisorTest, ConfirmationAndCapabilityAvailabilityAreCoreGates) {
  auto supervisor = make_supervisor();
  auto unavailable = capability("robot.recover");
  unavailable.available = false;
  unavailable.availability_reason = "recovery_cause_not_clear";
  ASSERT_TRUE(supervisor.replace_capabilities(
      1, 0, {capability(), unavailable}));
  ASSERT_TRUE(
      supervisor.establish_authority_session(authority_session(), kNow));

  auto no_confirmation = request_for(supervisor.snapshot(kNow));
  no_confirmation.confirmation_evidence.clear();
  EXPECT_EQ(
      supervisor.submit(no_confirmation, kNow).reason,
      "confirmation_required");

  auto recover =
    request_for(supervisor.snapshot(kNow), 2, "request-2", "robot.recover");
  EXPECT_EQ(
      supervisor.submit(recover, kNow).reason,
      "recovery_cause_not_clear");
}

TEST(SupervisorTest, SuccessRequiresExplicitExecutionAndVerification) {
  auto supervisor = make_supervisor();
  ASSERT_TRUE(supervisor.replace_capabilities(1, 0, {capability()}));
  ASSERT_TRUE(
      supervisor.establish_authority_session(authority_session(), kNow));
  const auto request = request_for(supervisor.snapshot(kNow));
  const auto decision = supervisor.submit(request, kNow);
  ASSERT_EQ(decision.disposition, CommandDisposition::kAccepted);

  EXPECT_FALSE(supervisor.finish_success(
      decision.transaction_generation, request.request_id,
      "not_verified", kNow + 1));
  ASSERT_TRUE(supervisor.mark_executing(
      decision.transaction_generation, request.request_id, kNow + 1));
  EXPECT_FALSE(supervisor.finish_success(
      decision.transaction_generation, request.request_id,
      "not_verified", kNow + 2));
  ASSERT_TRUE(supervisor.mark_verifying(
      decision.transaction_generation, request.request_id, kNow + 2));
  ASSERT_TRUE(supervisor.finish_success(
      decision.transaction_generation, request.request_id,
      "postcondition_observed", kNow + 3));

  const auto snapshot = supervisor.snapshot(kNow + 3);
  EXPECT_FALSE(snapshot.pending_transaction.has_value());
  ASSERT_TRUE(snapshot.last_terminal_transaction.has_value());
  EXPECT_EQ(
      snapshot.last_terminal_transaction->phase,
      TransactionPhase::kSucceeded);
  EXPECT_EQ(
      snapshot.last_terminal_transaction->result_reason,
      "postcondition_observed");
  const auto replay = supervisor.submit(request, kNow + 4);
  EXPECT_EQ(replay.disposition, CommandDisposition::kIdempotentReplay);
  EXPECT_EQ(replay.phase, TransactionPhase::kSucceeded);
  EXPECT_EQ(replay.reason, "postcondition_observed");
}

TEST(SupervisorTest, LateSuccessRequiresExplicitReconciliation) {
  auto supervisor = make_supervisor();
  auto short_capability = capability();
  short_capability.timeout_ns = 10;
  ASSERT_TRUE(
      supervisor.replace_capabilities(1, 0, {short_capability}));
  ASSERT_TRUE(
      supervisor.establish_authority_session(authority_session(), kNow));
  const auto request = request_for(supervisor.snapshot(kNow));
  const auto decision = supervisor.submit(request, kNow);
  ASSERT_EQ(decision.disposition, CommandDisposition::kAccepted);
  ASSERT_TRUE(supervisor.mark_executing(
      decision.transaction_generation, request.request_id, kNow + 1));
  ASSERT_TRUE(supervisor.mark_verifying(
      decision.transaction_generation, request.request_id, kNow + 2));

  EXPECT_FALSE(supervisor.finish_success(
      decision.transaction_generation, request.request_id,
      "late_postcondition", decision.deadline_ns));
  auto snapshot = supervisor.snapshot(decision.deadline_ns);
  ASSERT_TRUE(snapshot.pending_transaction.has_value());
  EXPECT_EQ(
      snapshot.pending_transaction->phase,
      TransactionPhase::kOutcomeUnknown);
  EXPECT_EQ(
      snapshot.pending_transaction->result_reason,
      "transaction_deadline_expired");
  EXPECT_TRUE(snapshot.reconciliation_required);

  const auto replay = supervisor.submit(request, decision.deadline_ns);
  EXPECT_EQ(replay.disposition, CommandDisposition::kIdempotentReplay);
  EXPECT_EQ(replay.phase, TransactionPhase::kOutcomeUnknown);
  ASSERT_TRUE(supervisor.resolve_uncertain(
      decision.transaction_generation, request.request_id, true,
      "late_postcondition_reconciled", decision.deadline_ns + 1));
  snapshot = supervisor.snapshot(decision.deadline_ns + 1);
  EXPECT_FALSE(snapshot.reconciliation_required);
  ASSERT_TRUE(snapshot.last_terminal_transaction.has_value());
  EXPECT_EQ(
      snapshot.last_terminal_transaction->phase,
      TransactionPhase::kSucceeded);
}

TEST(SupervisorTest, TimeoutAndAuthorityLossNeverRetryOrReleaseUncertainty) {
  auto supervisor = make_supervisor();
  ASSERT_TRUE(supervisor.replace_capabilities(1, 0, {capability()}));
  ASSERT_TRUE(
      supervisor.establish_authority_session(authority_session(), kNow));
  const auto request = request_for(supervisor.snapshot(kNow));
  const auto decision = supervisor.submit(request, kNow);
  ASSERT_EQ(decision.disposition, CommandDisposition::kAccepted);
  ASSERT_TRUE(supervisor.mark_executing(
      decision.transaction_generation, request.request_id, kNow + 1));

  ASSERT_TRUE(supervisor.advance_time(decision.deadline_ns));
  auto snapshot = supervisor.snapshot(decision.deadline_ns);
  ASSERT_TRUE(snapshot.pending_transaction.has_value());
  EXPECT_EQ(
      snapshot.pending_transaction->phase,
      TransactionPhase::kOutcomeUnknown);
  EXPECT_TRUE(snapshot.reconciliation_required);
  EXPECT_FALSE(snapshot.motion_admitted);

  EXPECT_FALSE(supervisor.finish_failure(
      decision.transaction_generation, request.request_id,
      "late_failure", decision.deadline_ns + 1));
  ASSERT_TRUE(supervisor.resolve_uncertain(
      decision.transaction_generation, request.request_id, false,
      "observed_postcondition_not_satisfied", decision.deadline_ns + 2));
  EXPECT_FALSE(
      supervisor.snapshot(decision.deadline_ns + 2)
    .reconciliation_required);

  const auto after_reconciliation_ns = decision.deadline_ns + 2;
  auto renewed_session = authority_session();
  renewed_session.generation = 2;
  renewed_session.session_id = "operator-session-2";
  renewed_session.valid_until_ns = after_reconciliation_ns + kLease;
  ASSERT_TRUE(
      supervisor.establish_authority_session(
          renewed_session, after_reconciliation_ns));
  auto next = request_for(
      supervisor.snapshot(after_reconciliation_ns), 1, "request-2");
  const auto next_decision =
    supervisor.submit(next, after_reconciliation_ns);
  ASSERT_EQ(next_decision.disposition, CommandDisposition::kAccepted);
  ASSERT_TRUE(supervisor.mark_executing(
      next_decision.transaction_generation, next.request_id,
      after_reconciliation_ns + 1));
  EXPECT_TRUE(supervisor.clear_authority_session(
      next.authority_session_id, next.authority_session_generation,
      after_reconciliation_ns + 2));
  snapshot = supervisor.snapshot(after_reconciliation_ns + 2);
  ASSERT_TRUE(snapshot.pending_transaction.has_value());
  EXPECT_EQ(
      snapshot.pending_transaction->phase,
      TransactionPhase::kOutcomeUnknown);
  EXPECT_FALSE(snapshot.authority_session.has_value());
  ASSERT_TRUE(supervisor.resolve_uncertain(
      next_decision.transaction_generation, next.request_id, false,
      "authority_loss_reconciled", after_reconciliation_ns + 3));
  EXPECT_FALSE(supervisor.establish_authority_session(
      renewed_session, after_reconciliation_ns + 3));
  auto replacement_session = renewed_session;
  replacement_session.generation = 3;
  replacement_session.session_id = "operator-session-3";
  replacement_session.valid_until_ns = after_reconciliation_ns + kLease;
  EXPECT_TRUE(supervisor.establish_authority_session(
      replacement_session, after_reconciliation_ns + 3));
}

TEST(SupervisorTest, AdapterCanFenceAnAttemptedPrimitiveWithUnknownOutcome) {
  auto supervisor = make_supervisor();
  ASSERT_TRUE(supervisor.replace_capabilities(1, 0, {capability()}));
  ASSERT_TRUE(
      supervisor.establish_authority_session(authority_session(), kNow));
  const auto request = request_for(supervisor.snapshot(kNow));
  const auto decision = supervisor.submit(request, kNow);
  ASSERT_EQ(decision.disposition, CommandDisposition::kAccepted);
  ASSERT_TRUE(supervisor.mark_executing(
      decision.transaction_generation, request.request_id, kNow + 1));

  ASSERT_TRUE(supervisor.mark_outcome_unknown(
      decision.transaction_generation, request.request_id,
      "vendor_result_ambiguous", kNow + 2));
  auto snapshot = supervisor.snapshot(kNow + 2);
  ASSERT_TRUE(snapshot.pending_transaction.has_value());
  EXPECT_EQ(
      snapshot.pending_transaction->phase,
      TransactionPhase::kOutcomeUnknown);
  EXPECT_TRUE(snapshot.reconciliation_required);
  EXPECT_FALSE(supervisor.finish_failure(
      decision.transaction_generation, request.request_id,
      "must_not_bypass_reconciliation", kNow + 3));

  ASSERT_TRUE(supervisor.resolve_uncertain(
      decision.transaction_generation, request.request_id, true,
      "postcondition_observed", kNow + 4));
  snapshot = supervisor.snapshot(kNow + 4);
  EXPECT_FALSE(snapshot.reconciliation_required);
  ASSERT_TRUE(snapshot.last_terminal_transaction.has_value());
  EXPECT_EQ(
      snapshot.last_terminal_transaction->phase,
      TransactionPhase::kSucceeded);
}

TEST(SupervisorTest, AuthorityProfilesAreMutuallyExclusive) {
  auto supervisor = make_supervisor(AuthorityProfile::kExternalPlc);
  EXPECT_FALSE(supervisor.establish_authority_session(
      authority_session(AuthorityProfile::kLocalOperator), kNow));
  ASSERT_TRUE(supervisor.establish_authority_session(
      authority_session(
          AuthorityProfile::kExternalPlc, 1, "plc-session-1"),
      kNow));

  const auto snapshot = supervisor.snapshot(kNow);
  EXPECT_EQ(snapshot.authority_profile, AuthorityProfile::kExternalPlc);
  ASSERT_TRUE(snapshot.authority_session.has_value());
  EXPECT_EQ(
      snapshot.authority_session->profile,
      AuthorityProfile::kExternalPlc);
}

TEST(SupervisorTest, InstancesFenceRobotIdentityAndState) {
  auto first = make_supervisor(
      AuthorityProfile::kLocalOperator, 128, "robot-1");
  auto second = make_supervisor(
      AuthorityProfile::kLocalOperator, 128, "robot-2");
  ASSERT_TRUE(first.replace_capabilities(1, 0, {capability()}));
  ASSERT_TRUE(second.replace_capabilities(1, 0, {capability()}));
  ASSERT_TRUE(first.establish_authority_session(authority_session(), kNow));
  ASSERT_TRUE(second.establish_authority_session(authority_session(), kNow));

  auto first_request = request_for(first.snapshot(kNow));
  EXPECT_EQ(
      second.submit(first_request, kNow).reason,
      "robot_identity_mismatch");
  EXPECT_EQ(
      first.submit(first_request, kNow).disposition,
      CommandDisposition::kAccepted);
  EXPECT_FALSE(first.snapshot(kNow).motion_admitted);
  EXPECT_FALSE(second.snapshot(kNow).pending_transaction.has_value());
}

TEST(SupervisorTest, RejectsRegressedObservationsAndCapabilitySets) {
  auto supervisor = make_supervisor();
  ASSERT_TRUE(supervisor.publish_observation(ready_observation(2)));
  EXPECT_FALSE(supervisor.publish_observation(ready_observation(1)));
  EXPECT_FALSE(supervisor.publish_observation(ready_observation(2)));
  ASSERT_TRUE(supervisor.replace_capabilities(2, 2, {capability()}));
  EXPECT_FALSE(supervisor.replace_capabilities(1, 2, {capability()}));

  auto invalid = capability();
  invalid.timeout_ns = 0;
  EXPECT_FALSE(supervisor.replace_capabilities(3, 2, {invalid}));
  EXPECT_EQ(supervisor.snapshot(kNow).capability_generation, 2u);
}

TEST(SupervisorTest, CapabilityAvailabilityIsBoundToExactObservation) {
  auto supervisor = make_supervisor();
  ASSERT_TRUE(supervisor.publish_observation(ready_observation(1)));
  ASSERT_TRUE(supervisor.replace_capabilities(1, 1, {capability()}));
  ASSERT_TRUE(
      supervisor.establish_authority_session(authority_session(), kNow));

  auto updated = ready_observation(2);
  updated.robot_enabled = FactState::kFalse;
  ASSERT_TRUE(supervisor.publish_observation(updated));
  auto stale_capability_request =
    request_for(supervisor.snapshot(kNow), 1, "request-1");
  EXPECT_EQ(
      supervisor.submit(stale_capability_request, kNow).reason,
      "capability_snapshot_stale");

  auto unavailable = capability();
  unavailable.available = false;
  unavailable.availability_reason = "robot_not_enabled";
  ASSERT_TRUE(supervisor.replace_capabilities(2, 2, {unavailable}));
  auto current_request =
    request_for(supervisor.snapshot(kNow), 2, "request-2");
  EXPECT_EQ(
      supervisor.submit(current_request, kNow).reason,
      "robot_not_enabled");
}

TEST(SupervisorTest, DeadlineBeforeExecutionHasKnownFailedOutcome) {
  auto supervisor = make_supervisor();
  auto short_capability = capability();
  short_capability.timeout_ns = 10;
  ASSERT_TRUE(
      supervisor.replace_capabilities(1, 0, {short_capability}));
  auto long_session = authority_session();
  long_session.valid_until_ns = kNow + kLease;
  ASSERT_TRUE(
      supervisor.establish_authority_session(long_session, kNow));
  const auto request = request_for(supervisor.snapshot(kNow));
  const auto decision = supervisor.submit(request, kNow);
  ASSERT_EQ(decision.disposition, CommandDisposition::kAccepted);

  EXPECT_TRUE(supervisor.advance_time(decision.deadline_ns));
  const auto snapshot = supervisor.snapshot(decision.deadline_ns);
  EXPECT_FALSE(snapshot.pending_transaction.has_value());
  EXPECT_FALSE(snapshot.reconciliation_required);
  ASSERT_TRUE(snapshot.last_terminal_transaction.has_value());
  EXPECT_EQ(
      snapshot.last_terminal_transaction->phase,
      TransactionPhase::kFailed);
  EXPECT_EQ(
      snapshot.last_terminal_transaction->result_reason,
      "transaction_not_started_before_deadline");
}

TEST(SupervisorTest, MonotonicTimeCannotBeRewoundToRestoreAuthority) {
  auto supervisor = make_supervisor();
  ASSERT_TRUE(supervisor.publish_observation(ready_observation(1)));
  ASSERT_TRUE(supervisor.replace_capabilities(1, 1, {capability()}));
  ASSERT_TRUE(
      supervisor.establish_authority_session(authority_session(), kNow));

  auto request = request_for(supervisor.snapshot(kNow));
  EXPECT_EQ(
      supervisor.submit(request, kNow - 1).reason,
      "time_regressed");
  EXPECT_FALSE(supervisor.advance_time(kNow + kLease));

  const auto rewound_snapshot = supervisor.snapshot(kNow);
  EXPECT_FALSE(rewound_snapshot.observation_fresh);
  EXPECT_FALSE(rewound_snapshot.authority_fresh);
  EXPECT_FALSE(rewound_snapshot.motion_admitted);

  auto late_observation = ready_observation(2);
  late_observation.received_timestamp_ns = kNow + kLease - 1;
  late_observation.fresh_until_ns = kNow + 2 * kLease;
  EXPECT_FALSE(supervisor.publish_observation(late_observation));
}

}  // namespace
}  // namespace manyforge::robot_lifecycle
