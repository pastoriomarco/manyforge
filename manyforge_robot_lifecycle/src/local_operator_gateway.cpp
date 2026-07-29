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

#include <manyforge_robot_lifecycle/local_operator_gateway.hpp>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace manyforge::robot_lifecycle
{
namespace
{

bool is_printable_identifier(std::string_view value)
{
  if (value.empty() || value.size() > 256) {
    return false;
  }
  return std::all_of(
    value.begin(), value.end(),
    [](unsigned char character) {
      return character >= 0x20 && character <= 0x7e;
    });
}

bool lease_exceeds_limit(
  std::int64_t now_ns,
  std::int64_t valid_until_ns,
  std::int64_t maximum_lease_ns)
{
  if (valid_until_ns <= now_ns) {
    return true;
  }
  if (now_ns >
    std::numeric_limits<std::int64_t>::max() - maximum_lease_ns)
  {
    return false;
  }
  return valid_until_ns > now_ns + maximum_lease_ns;
}

std::string bounded_audit_value(std::string_view value)
{
  constexpr std::size_t kMaximumAuditFieldLength = 256;
  return std::string(value.substr(0, kMaximumAuditFieldLength));
}

}  // namespace

LocalOperatorGateway::LocalOperatorGateway(
  LocalOperatorGatewayConfig config,
  LifecycleControlPort & control_port)
: config_(std::move(config)),
  control_port_(control_port)
{
  if (!is_printable_identifier(config_.robot_id) ||
    config_.maximum_authority_lease_ns <= 0 ||
    config_.audit_capacity == 0)
  {
    throw std::invalid_argument("local-operator gateway config is invalid");
  }
}

GatewaySnapshotResult LocalOperatorGateway::snapshot(
  const CallerContext & caller,
  std::int64_t now_ns)
{
  std::scoped_lock lock(mutex_);
  GatewaySnapshotResult result;
  if (!validate_caller(
      caller, now_ns, result.reason, result.caller_context_valid))
  {
    return result;
  }
  result.reason = "snapshot_returned";
  result.snapshot = control_port_.lifecycle_snapshot(now_ns);
  return result;
}

GatewayMutationResult LocalOperatorGateway::open_session(
  const CallerContext & caller,
  const LocalAuthoritySessionIntent & intent,
  std::int64_t now_ns)
{
  std::scoped_lock lock(mutex_);
  GatewayMutationResult result;
  if (!validate_caller(
      caller, now_ns, result.reason, result.caller_context_valid))
  {
    record(caller, "open_session", intent.session_id, result, now_ns);
    return result;
  }
  if (!is_printable_identifier(intent.session_id) ||
    !is_printable_identifier(intent.component_epoch) ||
    intent.generation == 0 ||
    lease_exceeds_limit(
      now_ns, intent.valid_until_ns,
      config_.maximum_authority_lease_ns))
  {
    result.reason = "authority_session_intent_invalid";
  } else if (intent.valid_until_ns > caller.valid_until_ns) {
    result.reason = "authority_session_outlives_caller_context";
  } else {
    const auto current = control_port_.lifecycle_snapshot(now_ns);
    if (current.authority_fresh &&
      current.authority_session.has_value() &&
      !caller_owns_session(
        caller, current, intent.session_id, intent.generation))
    {
      result.reason = "authority_session_already_owned";
    } else {
      AuthoritySession session;
      session.profile = AuthorityProfile::kLocalOperator;
      session.session_id = intent.session_id;
      session.generation = intent.generation;
      session.principal_id = caller.command_source;
      session.valid_until_ns = intent.valid_until_ns;
      session.component_epoch = intent.component_epoch;
      result.accepted =
        control_port_.establish_authority_session(session, now_ns);
      result.reason = result.accepted ?
        "authority_session_established" :
        "authority_session_rejected";
      if (result.accepted) {
        active_session_binding_ = ActiveSessionBinding{
          intent.session_id,
          intent.generation,
          caller.command_source,
          caller.caller_context_id,
          intent.component_epoch};
      }
    }
  }
  record(caller, "open_session", intent.session_id, result, now_ns);
  return result;
}

GatewayMutationResult LocalOperatorGateway::close_session(
  const CallerContext & caller,
  std::string_view session_id,
  std::uint64_t session_generation,
  std::int64_t now_ns)
{
  std::scoped_lock lock(mutex_);
  GatewayMutationResult result;
  if (!validate_caller(
      caller, now_ns, result.reason, result.caller_context_valid))
  {
    record(
      caller, "close_session", std::string(session_id), result, now_ns);
    return result;
  }
  const auto current = control_port_.lifecycle_snapshot(now_ns);
  if (!caller_owns_session(
      caller, current, session_id, session_generation))
  {
    result.reason = "authority_session_not_owned";
  } else {
    result.accepted = control_port_.clear_authority_session(
      session_id, session_generation, now_ns);
    result.reason = result.accepted ?
      "authority_session_closed" :
      "authority_session_close_rejected";
    if (result.accepted) {
      active_session_binding_.reset();
    }
  }
  record(
    caller, "close_session", std::string(session_id), result, now_ns);
  return result;
}

GatewayMutationResult LocalOperatorGateway::submit(
  const CallerContext & caller,
  const LocalLifecycleCommandIntent & intent,
  std::int64_t now_ns)
{
  std::scoped_lock lock(mutex_);
  GatewayMutationResult result;
  if (!validate_caller(
      caller, now_ns, result.reason, result.caller_context_valid))
  {
    record(caller, "submit", intent.request_id, result, now_ns);
    return result;
  }
  const auto current = control_port_.lifecycle_snapshot(now_ns);
  if (!caller_owns_session(
      caller, current, intent.authority_session_id,
      intent.authority_session_generation))
  {
    result.reason = "authority_session_not_owned";
    record(caller, "submit", intent.request_id, result, now_ns);
    return result;
  }

  CommandRequest request;
  request.robot_id = config_.robot_id;
  request.request_id = intent.request_id;
  request.request_sequence = intent.request_sequence;
  request.action_id = intent.action_id;
  request.authority_profile = AuthorityProfile::kLocalOperator;
  request.authority_session_id = intent.authority_session_id;
  request.authority_session_generation =
    intent.authority_session_generation;
  request.principal_id = caller.command_source;
  request.expected_snapshot_generation =
    intent.expected_snapshot_generation;
  request.confirmation_evidence = intent.confirmation_evidence;

  result.command_decision =
    control_port_.submit_lifecycle(request, now_ns);
  result.accepted =
    result.command_decision->disposition != CommandDisposition::kRejected;
  result.reason = result.command_decision->reason;
  record(caller, "submit", intent.request_id, result, now_ns);
  return result;
}

GatewayMutationResult LocalOperatorGateway::reconcile(
  const CallerContext & caller,
  std::uint64_t transaction_generation,
  std::string_view request_id,
  std::string reason,
  std::int64_t now_ns)
{
  std::scoped_lock lock(mutex_);
  GatewayMutationResult result;
  if (!validate_caller(
      caller, now_ns, result.reason, result.caller_context_valid))
  {
    record(
      caller, "reconcile", std::string(request_id), result, now_ns);
    return result;
  }
  if (!is_printable_identifier(reason)) {
    result.reason = "reconciliation_reason_invalid";
    record(
      caller, "reconcile", std::string(request_id), result, now_ns);
    return result;
  }
  const auto current = control_port_.lifecycle_snapshot(now_ns);
  if (!current.authority_session.has_value() ||
    !caller_owns_session(
      caller, current, current.authority_session->session_id,
      current.authority_session->generation))
  {
    result.reason = "authority_session_not_owned";
  } else {
    result.accepted = control_port_.reconcile_lifecycle(
      transaction_generation, request_id, std::move(reason), now_ns);
    result.reason = result.accepted ?
      "transaction_reconciled" :
      "transaction_reconcile_rejected";
  }
  record(
    caller, "reconcile", std::string(request_id), result, now_ns);
  return result;
}

std::vector<GatewayAuditRecord>
LocalOperatorGateway::audit_snapshot() const
{
  std::scoped_lock lock(mutex_);
  return {audit_.begin(), audit_.end()};
}

bool LocalOperatorGateway::validate_caller(
  const CallerContext & caller,
  std::int64_t now_ns,
  std::string & reason,
  bool & caller_context_valid) const
{
  caller_context_valid = false;
  if (!is_printable_identifier(caller.command_source) ||
    !is_printable_identifier(caller.caller_context_id) ||
    caller.asserted_at_ns > now_ns ||
    caller.valid_until_ns <= now_ns)
  {
    reason = "caller_context_invalid_or_stale";
    return false;
  }
  caller_context_valid = true;
  if (static_cast<int>(caller.assurance) <
    static_cast<int>(config_.minimum_assurance))
  {
    reason = "caller_assurance_insufficient";
    return false;
  }
  return true;
}

bool LocalOperatorGateway::caller_owns_session(
  const CallerContext & caller,
  const LifecycleSnapshot & snapshot,
  std::string_view session_id,
  std::uint64_t session_generation) const
{
  return snapshot.robot_id == config_.robot_id &&
         snapshot.authority_profile ==
         AuthorityProfile::kLocalOperator &&
         active_session_binding_.has_value() &&
         active_session_binding_->session_id == session_id &&
         active_session_binding_->session_generation ==
         session_generation &&
         active_session_binding_->command_source == caller.command_source &&
         active_session_binding_->caller_context_id ==
         caller.caller_context_id &&
         snapshot.authority_session.has_value() &&
         snapshot.authority_session->component_epoch ==
         active_session_binding_->component_epoch &&
         snapshot.authority_session->session_id == session_id &&
         snapshot.authority_session->generation == session_generation &&
         snapshot.authority_session->principal_id == caller.command_source;
}

void LocalOperatorGateway::record(
  const CallerContext & caller,
  std::string operation,
  std::string target_id,
  const GatewayMutationResult & result,
  std::int64_t now_ns)
{
  GatewayAuditRecord entry;
  entry.sequence = next_audit_sequence_++;
  entry.occurred_at_ns = now_ns;
  entry.command_source = bounded_audit_value(caller.command_source);
  entry.caller_context_id =
    bounded_audit_value(caller.caller_context_id);
  entry.caller_assurance = caller.assurance;
  entry.operation = std::move(operation);
  entry.target_id = bounded_audit_value(target_id);
  entry.caller_context_valid = result.caller_context_valid;
  entry.accepted = result.accepted;
  entry.reason = bounded_audit_value(result.reason);
  audit_.push_back(std::move(entry));
  while (audit_.size() > config_.audit_capacity) {
    audit_.pop_front();
  }
}

}  // namespace manyforge::robot_lifecycle
