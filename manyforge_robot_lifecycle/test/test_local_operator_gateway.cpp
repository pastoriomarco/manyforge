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

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include <manyforge_robot_lifecycle/local_operator_gateway.hpp>

namespace manyforge::robot_lifecycle
{
namespace
{

constexpr std::int64_t kNow = 10'000'000'000;

class RecordingControlPort final : public LifecycleControlPort
{
public:
  RecordingControlPort()
  {
    snapshot_.robot_id = "lite6-a";
    snapshot_.authority_profile = AuthorityProfile::kLocalOperator;
    snapshot_.generation = 7;
  }

  bool establish_authority_session(
    const AuthoritySession & session,
    std::int64_t) override
  {
    ++establish_count;
    snapshot_.authority_session = session;
    ++snapshot_.generation;
    return true;
  }

  bool clear_authority_session(
    std::string_view session_id,
    std::uint64_t session_generation,
    std::int64_t) override
  {
    ++clear_count;
    if (!snapshot_.authority_session.has_value() ||
      snapshot_.authority_session->session_id != session_id ||
      snapshot_.authority_session->generation != session_generation)
    {
      return false;
    }
    snapshot_.authority_session.reset();
    ++snapshot_.generation;
    return true;
  }

  CommandDecision submit_lifecycle(
    const CommandRequest & request,
    std::int64_t) override
  {
    ++submit_count;
    last_request = request;
    CommandDecision decision;
    decision.disposition = CommandDisposition::kAccepted;
    decision.reason = "accepted_by_recording_port";
    decision.phase = TransactionPhase::kSucceeded;
    return decision;
  }

  bool reconcile_lifecycle(
    std::uint64_t,
    std::string_view,
    std::string,
    std::int64_t) override
  {
    ++reconcile_count;
    return true;
  }

  bool refresh_lifecycle(std::int64_t) override
  {
    return true;
  }

  LifecycleSnapshot lifecycle_snapshot(std::int64_t now_ns) const override
  {
    auto result = snapshot_;
    result.authority_fresh =
      result.authority_session.has_value() &&
      result.authority_session->valid_until_ns > now_ns;
    return result;
  }

  LifecycleSnapshot snapshot_;
  std::optional<CommandRequest> last_request;
  std::size_t establish_count = 0;
  std::size_t clear_count = 0;
  std::size_t submit_count = 0;
  std::size_t reconcile_count = 0;
};

CallerContext operator_caller(
  std::string command_source = "composer.local_operator")
{
  CallerContext result;
  result.command_source = std::move(command_source);
  result.caller_context_id = "composer-process:42";
  result.assurance = CallerAssurance::kAuthenticated;
  result.asserted_at_ns = kNow - 1;
  result.valid_until_ns = kNow + 60'000'000'000;
  return result;
}

LocalOperatorGateway gateway(
  RecordingControlPort & port,
  std::size_t audit_capacity = 8)
{
  LocalOperatorGatewayConfig config;
  config.robot_id = "lite6-a";
  config.maximum_authority_lease_ns = 30'000'000'000;
  config.audit_capacity = audit_capacity;
  return LocalOperatorGateway(std::move(config), port);
}

LocalAuthoritySessionIntent session_intent()
{
  LocalAuthoritySessionIntent result;
  result.session_id = "local-session-1";
  result.generation = 1;
  result.valid_until_ns = kNow + 20'000'000'000;
  result.component_epoch = "local-authority-epoch-1";
  return result;
}

TEST(LocalOperatorGatewayTest, RejectsStaleAndInsufficientAssurance)
{
  RecordingControlPort port;
  auto target = gateway(port);

  auto stale = operator_caller();
  stale.valid_until_ns = kNow;
  const auto stale_result =
    target.open_session(stale, session_intent(), kNow);
  EXPECT_FALSE(stale_result.caller_context_valid);
  EXPECT_FALSE(stale_result.accepted);

  auto unverified = operator_caller();
  unverified.assurance = CallerAssurance::kUnverified;
  const auto unverified_result =
    target.open_session(unverified, session_intent(), kNow);
  EXPECT_TRUE(unverified_result.caller_context_valid);
  EXPECT_FALSE(unverified_result.accepted);
  EXPECT_EQ(
    unverified_result.reason, "caller_assurance_insufficient");
  EXPECT_EQ(port.establish_count, 0U);

  const auto visible = target.snapshot(operator_caller(), kNow);
  EXPECT_TRUE(visible.caller_context_valid);
  ASSERT_TRUE(visible.snapshot.has_value());
  EXPECT_EQ(visible.snapshot->robot_id, "lite6-a");
}

TEST(LocalOperatorGatewayTest, BindsServerAuthoritativeIdentityAndProfile)
{
  RecordingControlPort port;
  auto target = gateway(port);
  const auto caller = operator_caller();
  ASSERT_TRUE(
    target.open_session(caller, session_intent(), kNow).accepted);

  LocalLifecycleCommandIntent intent;
  intent.request_id = "request-1";
  intent.request_sequence = 1;
  intent.action_id = "robot.enable";
  intent.authority_session_id = "local-session-1";
  intent.authority_session_generation = 1;
  intent.expected_snapshot_generation = port.snapshot_.generation;
  intent.confirmation_evidence = "operator-confirmed";
  const auto result = target.submit(caller, intent, kNow + 1);

  EXPECT_TRUE(result.caller_context_valid);
  EXPECT_TRUE(result.accepted);
  ASSERT_TRUE(port.last_request.has_value());
  EXPECT_EQ(port.last_request->robot_id, "lite6-a");
  EXPECT_EQ(
    port.last_request->authority_profile,
    AuthorityProfile::kLocalOperator);
  EXPECT_EQ(port.last_request->principal_id, "composer.local_operator");
  EXPECT_EQ(port.last_request->request_id, "request-1");

  const auto audit = target.audit_snapshot();
  ASSERT_EQ(audit.size(), 2U);
  EXPECT_EQ(audit[0].operation, "open_session");
  EXPECT_EQ(audit[1].operation, "submit");
  EXPECT_EQ(
    audit[1].caller_context_id, "composer-process:42");
}

TEST(
  LocalOperatorGatewayTest,
  SameCallerRenewsOwnedSessionWithoutClosingOrChangingGeneration)
{
  RecordingControlPort port;
  auto target = gateway(port);
  const auto caller = operator_caller();
  const auto initial = session_intent();
  ASSERT_TRUE(target.open_session(caller, initial, kNow).accepted);

  auto renewed = initial;
  renewed.valid_until_ns = kNow + 25'000'000'000;
  const auto result =
    target.open_session(caller, renewed, kNow + 1);

  EXPECT_TRUE(result.accepted);
  EXPECT_EQ(port.establish_count, 2U);
  EXPECT_EQ(port.clear_count, 0U);
  ASSERT_TRUE(port.snapshot_.authority_session.has_value());
  EXPECT_EQ(
    port.snapshot_.authority_session->generation,
    initial.generation);
  EXPECT_EQ(
    port.snapshot_.authority_session->valid_until_ns,
    renewed.valid_until_ns);
}

TEST(LocalOperatorGatewayTest, PreventsSessionHijackAndOverlongLease)
{
  RecordingControlPort port;
  auto target = gateway(port);
  const auto owner = operator_caller();
  ASSERT_TRUE(
    target.open_session(owner, session_intent(), kNow).accepted);

  const auto invalid_reconciliation = target.reconcile(
    owner, 1, "request-1", "", kNow + 1);
  EXPECT_FALSE(invalid_reconciliation.accepted);
  EXPECT_EQ(
    invalid_reconciliation.reason, "reconciliation_reason_invalid");
  EXPECT_EQ(port.reconcile_count, 0U);

  const auto intruder = operator_caller("composer.other_process");
  const auto close_result = target.close_session(
    intruder, "local-session-1", 1, kNow + 2);
  EXPECT_TRUE(close_result.caller_context_valid);
  EXPECT_FALSE(close_result.accepted);
  EXPECT_EQ(close_result.reason, "authority_session_not_owned");
  EXPECT_EQ(port.clear_count, 0U);

  auto replacement = session_intent();
  replacement.session_id = "local-session-2";
  replacement.generation = 2;
  const auto replacement_result =
    target.open_session(intruder, replacement, kNow + 3);
  EXPECT_FALSE(replacement_result.accepted);
  EXPECT_EQ(
    replacement_result.reason, "authority_session_already_owned");
  EXPECT_EQ(port.establish_count, 1U);

  auto second_context = owner;
  second_context.caller_context_id = "composer-process:99";
  const auto second_context_result = target.close_session(
    second_context, "local-session-1", 1, kNow + 4);
  EXPECT_FALSE(second_context_result.accepted);
  EXPECT_EQ(
    second_context_result.reason, "authority_session_not_owned");
  EXPECT_EQ(port.clear_count, 0U);

  auto overlong = session_intent();
  overlong.session_id = "local-session-3";
  overlong.generation = 3;
  overlong.valid_until_ns = kNow + 31'000'000'000;
  const auto overlong_result =
    target.open_session(owner, overlong, kNow + 5);
  EXPECT_FALSE(overlong_result.accepted);
  EXPECT_EQ(
    overlong_result.reason, "authority_session_intent_invalid");
}

TEST(LocalOperatorGatewayTest, AuditTrailIsBounded)
{
  RecordingControlPort port;
  auto target = gateway(port, 2);
  auto unverified = operator_caller();
  unverified.assurance = CallerAssurance::kUnverified;

  target.open_session(unverified, session_intent(), kNow);
  target.open_session(unverified, session_intent(), kNow + 1);
  target.open_session(unverified, session_intent(), kNow + 2);

  const auto audit = target.audit_snapshot();
  ASSERT_EQ(audit.size(), 2U);
  EXPECT_EQ(audit[0].sequence, 2U);
  EXPECT_EQ(audit[1].sequence, 3U);
}

}  // namespace
}  // namespace manyforge::robot_lifecycle
