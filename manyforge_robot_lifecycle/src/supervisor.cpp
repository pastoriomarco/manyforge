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

#include <manyforge_robot_lifecycle/supervisor.hpp>

#include <algorithm>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace manyforge::robot_lifecycle
{
namespace
{

bool is_nonempty_identifier(std::string_view value)
{
  if (value.empty() || value.size() > 128) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](const unsigned char character) {
             return character >= 0x20 && character <= 0x7e;
  });
}

bool is_true(FactState value)
{
  return value == FactState::kTrue;
}

bool is_false(FactState value)
{
  return value == FactState::kFalse;
}

bool add_would_overflow(std::int64_t left, std::int64_t right)
{
  return right > 0 && left > std::numeric_limits<std::int64_t>::max() - right;
}

void add_reason(std::vector<std::string> & reasons, std::string reason)
{
  if (std::find(reasons.begin(), reasons.end(), reason) == reasons.end()) {
    reasons.push_back(std::move(reason));
  }
}

}  // namespace

const char * authority_profile_name(AuthorityProfile profile) noexcept
{
  switch (profile) {
    case AuthorityProfile::kLocalOperator:
      return "local_operator";
    case AuthorityProfile::kExternalPlc:
      return "external_plc";
  }
  return "unknown";
}

const char * fact_state_name(FactState state) noexcept
{
  switch (state) {
    case FactState::kUnknown:
      return "unknown";
    case FactState::kFalse:
      return "false";
    case FactState::kTrue:
      return "true";
  }
  return "unknown";
}

const char * confirmation_class_name(ConfirmationClass confirmation) noexcept
{
  switch (confirmation) {
    case ConfirmationClass::kNone:
      return "none";
    case ConfirmationClass::kExplicit:
      return "explicit";
    case ConfirmationClass::kHighConsequence:
      return "high_consequence";
  }
  return "unknown";
}

const char * transaction_phase_name(TransactionPhase phase) noexcept
{
  switch (phase) {
    case TransactionPhase::kAccepted:
      return "accepted";
    case TransactionPhase::kExecuting:
      return "executing";
    case TransactionPhase::kVerifying:
      return "verifying";
    case TransactionPhase::kSucceeded:
      return "succeeded";
    case TransactionPhase::kFailed:
      return "failed";
    case TransactionPhase::kOutcomeUnknown:
      return "outcome_unknown";
  }
  return "unknown";
}

const char * command_disposition_name(CommandDisposition disposition) noexcept
{
  switch (disposition) {
    case CommandDisposition::kAccepted:
      return "accepted";
    case CommandDisposition::kIdempotentReplay:
      return "idempotent_replay";
    case CommandDisposition::kRejected:
      return "rejected";
  }
  return "unknown";
}

Supervisor::Supervisor(SupervisorConfig config)
: config_(std::move(config))
{
  if (!is_nonempty_identifier(config_.robot_id)) {
    throw std::invalid_argument("robot_id must be a non-empty printable identifier");
  }
  if (!is_nonempty_identifier(config_.supervisor_instance_id)) {
    throw std::invalid_argument(
        "supervisor_instance_id must be a non-empty printable identifier");
  }
  if (config_.replay_capacity == 0) {
    throw std::invalid_argument("replay_capacity must be positive");
  }
}

bool Supervisor::publish_observation(const ObservedFacets & observation)
{
  if (observation.generation <= observed_.generation ||
    observation.received_timestamp_ns < observation.source_timestamp_ns ||
    observation.fresh_until_ns < observation.received_timestamp_ns ||
    !accept_time(observation.received_timestamp_ns))
  {
    return false;
  }
  observed_ = observation;
  bump_generation();
  return true;
}

bool Supervisor::renew_observation_lease(
  std::uint64_t observation_generation,
  std::int64_t source_timestamp_ns,
  std::int64_t received_timestamp_ns,
  std::int64_t fresh_until_ns)
{
  if (observation_generation == 0 ||
    observation_generation != observed_.generation ||
    source_timestamp_ns < observed_.source_timestamp_ns ||
    received_timestamp_ns < observed_.received_timestamp_ns ||
    received_timestamp_ns < source_timestamp_ns ||
    observed_.fresh_until_ns <= received_timestamp_ns ||
    fresh_until_ns <= received_timestamp_ns ||
    !accept_time(received_timestamp_ns))
  {
    return false;
  }
  observed_.source_timestamp_ns = source_timestamp_ns;
  observed_.received_timestamp_ns = received_timestamp_ns;
  observed_.fresh_until_ns = fresh_until_ns;
  return true;
}

bool Supervisor::replace_capabilities(
  std::uint64_t capability_generation,
  std::uint64_t observation_generation,
  std::vector<CapabilityDescriptor> capabilities)
{
  if (capability_generation <= capability_generation_ ||
    observation_generation != observed_.generation)
  {
    return false;
  }

  std::unordered_set<std::string> action_ids;
  for (const auto & capability : capabilities) {
    if (!is_nonempty_identifier(capability.action_id) ||
      !action_ids.insert(capability.action_id).second ||
      capability.label.empty())
    {
      return false;
    }
    if (capability.available && !capability.supported) {
      return false;
    }
    if (capability.supported &&
      (capability.timeout_ns <= 0 ||
      capability.expected_postcondition.empty() ||
      capability.side_effect_summary.empty()))
    {
      return false;
    }
  }

  capability_generation_ = capability_generation;
  capability_observation_generation_ = observation_generation;
  capabilities_ = std::move(capabilities);
  bump_generation();
  return true;
}

bool Supervisor::establish_authority_session(
  const AuthoritySession & session, std::int64_t now_ns)
{
  if (!accept_time(now_ns)) {
    return false;
  }
  if (session.profile != config_.authority_profile ||
    !is_nonempty_identifier(session.session_id) ||
    !is_nonempty_identifier(session.principal_id) ||
    !is_nonempty_identifier(session.component_epoch) ||
    session.generation == 0 || session.valid_until_ns <= now_ns)
  {
    return false;
  }

  if (!authority_session_.has_value() &&
    session.generation <= highest_authority_session_generation_)
  {
    return false;
  }

  if (authority_session_.has_value()) {
    if (session.generation < authority_session_->generation) {
      return false;
    }
    if (session.generation == authority_session_->generation) {
      if (session.session_id != authority_session_->session_id ||
        session.principal_id != authority_session_->principal_id ||
        session.component_epoch != authority_session_->component_epoch ||
        authority_session_->valid_until_ns <= now_ns ||
        session.valid_until_ns < authority_session_->valid_until_ns)
      {
        return false;
      }
      if (session.valid_until_ns == authority_session_->valid_until_ns) {
        return true;
      }
      authority_session_->valid_until_ns = session.valid_until_ns;
      bump_generation();
      return true;
    }
    if (pending_transaction_.has_value()) {
      handle_authority_loss("authority_session_replaced", now_ns);
      return false;
    }
  }

  authority_session_ = session;
  highest_authority_session_generation_ = session.generation;
  highest_request_sequence_ = 0;
  replay_.clear();
  replay_order_.clear();
  bump_generation();
  return true;
}

bool Supervisor::clear_authority_session(
  std::string_view session_id,
  std::uint64_t session_generation,
  std::int64_t now_ns)
{
  if (!accept_time(now_ns)) {
    return false;
  }
  if (!authority_session_.has_value() ||
    authority_session_->session_id != session_id ||
    authority_session_->generation != session_generation)
  {
    return false;
  }
  if (pending_transaction_.has_value()) {
    handle_authority_loss("authority_session_lost", now_ns);
  }
  authority_session_.reset();
  highest_request_sequence_ = 0;
  replay_.clear();
  replay_order_.clear();
  bump_generation();
  return true;
}

CommandDecision Supervisor::submit(
  const CommandRequest & request, std::int64_t now_ns)
{
  if (!accept_time(now_ns)) {
    return reject(request, "time_regressed", false);
  }
  if (request.robot_id != config_.robot_id) {
    return reject(request, "robot_identity_mismatch", false);
  }
  if (request.authority_profile != config_.authority_profile) {
    return reject(request, "authority_profile_mismatch", false);
  }
  if (!authority_session_.has_value()) {
    return reject(request, "authority_session_missing", false);
  }
  if (request.authority_session_id != authority_session_->session_id ||
    request.authority_session_generation != authority_session_->generation ||
    request.principal_id != authority_session_->principal_id)
  {
    return reject(request, "authority_session_mismatch", false);
  }
  if (authority_session_->valid_until_ns <= now_ns) {
    return reject(request, "authority_session_stale", false);
  }
  if (!is_nonempty_identifier(request.request_id) ||
    !is_nonempty_identifier(request.action_id) ||
    request.request_sequence == 0 ||
    (!request.confirmation_evidence.empty() &&
    !is_nonempty_identifier(request.confirmation_evidence)))
  {
    return reject(request, "request_invalid", false);
  }

  const auto fingerprint = request_fingerprint(request);
  if (const auto replay = replay_.find(request.request_sequence);
    replay != replay_.end())
  {
    if (replay->second.fingerprint != fingerprint) {
      return reject(request, "request_sequence_conflict", false);
    }
    CommandDecision decision = replay->second.decision;
    decision.disposition = CommandDisposition::kIdempotentReplay;
    return decision;
  }
  if (request.request_sequence <= highest_request_sequence_) {
    return reject(request, "request_sequence_retired", false);
  }
  if (request.request_sequence != highest_request_sequence_ + 1) {
    return reject(request, "request_sequence_gap", false);
  }

  if (request.expected_snapshot_generation != snapshot_generation_) {
    return reject(request, "snapshot_generation_mismatch", true);
  }
  if (pending_transaction_.has_value()) {
    return reject(request, "transaction_pending", true);
  }
  if (capability_observation_generation_ != observed_.generation) {
    return reject(request, "capability_snapshot_stale", true);
  }
  const CapabilityDescriptor * capability = find_capability(request.action_id);
  if (capability == nullptr) {
    return reject(request, "capability_unknown", true);
  }
  if (!capability->supported) {
    return reject(request, "capability_unsupported", true);
  }
  if (!capability->available) {
    return reject(
        request,
        capability->availability_reason.empty() ?
              "capability_unavailable" :
              capability->availability_reason,
        true);
  }
  if (capability->confirmation != ConfirmationClass::kNone &&
    request.confirmation_evidence.empty())
  {
    return reject(request, "confirmation_required", true);
  }
  if (add_would_overflow(now_ns, capability->timeout_ns)) {
    return reject(request, "deadline_overflow", true);
  }

  TransactionRecord transaction;
  transaction.generation = next_transaction_generation_++;
  transaction.request_id = request.request_id;
  transaction.request_sequence = request.request_sequence;
  transaction.action_id = request.action_id;
  transaction.principal_id = request.principal_id;
  transaction.authority_session_generation =
    request.authority_session_generation;
  transaction.phase = TransactionPhase::kAccepted;
  transaction.accepted_at_ns = now_ns;
  transaction.deadline_ns = now_ns + capability->timeout_ns;
  pending_transaction_ = transaction;
  bump_generation();

  CommandDecision decision;
  decision.disposition = CommandDisposition::kAccepted;
  decision.reason = "accepted";
  decision.transaction_generation = transaction.generation;
  decision.phase = transaction.phase;
  decision.deadline_ns = transaction.deadline_ns;
  remember(request.request_sequence, fingerprint, decision);
  return decision;
}

bool Supervisor::mark_executing(
  std::uint64_t transaction_generation,
  std::string_view request_id,
  std::int64_t now_ns)
{
  if (!accept_time(now_ns)) {
    return false;
  }
  if (!pending_matches(transaction_generation, request_id) ||
    pending_transaction_->phase != TransactionPhase::kAccepted)
  {
    return false;
  }
  if (!authority_session_.has_value() ||
    authority_session_->valid_until_ns <= now_ns)
  {
    finish_terminal(
      transaction_generation, request_id, TransactionPhase::kFailed,
      "authority_session_stale_before_execution", now_ns);
    return false;
  }
  if (now_ns >= pending_transaction_->deadline_ns) {
    finish_terminal(
      transaction_generation, request_id, TransactionPhase::kFailed,
      "transaction_not_started_before_deadline", now_ns);
    return false;
  }
  pending_transaction_->phase = TransactionPhase::kExecuting;
  update_replay_decision(*pending_transaction_);
  bump_generation();
  return true;
}

bool Supervisor::mark_verifying(
  std::uint64_t transaction_generation,
  std::string_view request_id,
  std::int64_t now_ns)
{
  if (!accept_time(now_ns)) {
    return false;
  }
  if (!pending_matches(transaction_generation, request_id) ||
    pending_transaction_->phase != TransactionPhase::kExecuting)
  {
    return false;
  }
  if (!authority_session_.has_value() ||
    authority_session_->valid_until_ns <= now_ns)
  {
    mark_pending_outcome_unknown("authority_session_stale", now_ns);
    return false;
  }
  if (now_ns >= pending_transaction_->deadline_ns) {
    mark_pending_outcome_unknown("transaction_deadline_expired", now_ns);
    return false;
  }
  pending_transaction_->phase = TransactionPhase::kVerifying;
  update_replay_decision(*pending_transaction_);
  bump_generation();
  return true;
}

bool Supervisor::finish_success(
  std::uint64_t transaction_generation,
  std::string_view request_id,
  std::string reason,
  std::int64_t now_ns)
{
  if (!accept_time(now_ns)) {
    return false;
  }
  if (!pending_matches(transaction_generation, request_id) ||
    pending_transaction_->phase != TransactionPhase::kVerifying)
  {
    return false;
  }
  if (!authority_session_.has_value() ||
    authority_session_->valid_until_ns <= now_ns)
  {
    mark_pending_outcome_unknown("authority_session_stale", now_ns);
    return false;
  }
  if (now_ns >= pending_transaction_->deadline_ns) {
    mark_pending_outcome_unknown("transaction_deadline_expired", now_ns);
    return false;
  }
  return finish_terminal(
      transaction_generation, request_id, TransactionPhase::kSucceeded,
      std::move(reason), now_ns);
}

bool Supervisor::finish_failure(
  std::uint64_t transaction_generation,
  std::string_view request_id,
  std::string reason,
  std::int64_t now_ns)
{
  if (!accept_time(now_ns)) {
    return false;
  }
  if (!pending_matches(transaction_generation, request_id) ||
    pending_transaction_->phase == TransactionPhase::kOutcomeUnknown)
  {
    return false;
  }
  return finish_terminal(
      transaction_generation, request_id, TransactionPhase::kFailed,
      std::move(reason), now_ns);
}

bool Supervisor::mark_outcome_unknown(
  std::uint64_t transaction_generation,
  std::string_view request_id,
  std::string reason,
  std::int64_t now_ns)
{
  if (!accept_time(now_ns)) {
    return false;
  }
  if (!pending_matches(transaction_generation, request_id) ||
    (pending_transaction_->phase != TransactionPhase::kExecuting &&
    pending_transaction_->phase != TransactionPhase::kVerifying))
  {
    return false;
  }
  mark_pending_outcome_unknown(std::move(reason), now_ns);
  return true;
}

bool Supervisor::resolve_uncertain(
  std::uint64_t transaction_generation,
  std::string_view request_id,
  bool postcondition_satisfied,
  std::string reason,
  std::int64_t now_ns)
{
  if (!accept_time(now_ns)) {
    return false;
  }
  if (!pending_matches(transaction_generation, request_id) ||
    pending_transaction_->phase != TransactionPhase::kOutcomeUnknown)
  {
    return false;
  }
  return finish_terminal(
      transaction_generation, request_id,
      postcondition_satisfied ? TransactionPhase::kSucceeded :
                                TransactionPhase::kFailed,
      std::move(reason), now_ns);
}

bool Supervisor::advance_time(std::int64_t now_ns)
{
  if (!accept_time(now_ns)) {
    return false;
  }
  if (!pending_transaction_.has_value() ||
    pending_transaction_->phase == TransactionPhase::kOutcomeUnknown)
  {
    return false;
  }
  if (!authority_session_.has_value() ||
    authority_session_->valid_until_ns <= now_ns)
  {
    handle_authority_loss("authority_session_stale", now_ns);
    return true;
  }
  if (now_ns < pending_transaction_->deadline_ns) {
    return false;
  }
  if (pending_transaction_->phase == TransactionPhase::kAccepted) {
    return finish_terminal(
      pending_transaction_->generation, pending_transaction_->request_id,
      TransactionPhase::kFailed, "transaction_not_started_before_deadline",
      now_ns);
  }
  mark_pending_outcome_unknown("transaction_deadline_expired", now_ns);
  return true;
}

bool Supervisor::motion_admitted(std::int64_t now_ns) const noexcept
{
  const std::int64_t evaluation_time_ns =
    std::max(now_ns, last_transition_time_ns_);
  const bool observation_fresh =
    observed_.generation > 0 &&
    observed_.received_timestamp_ns <= evaluation_time_ns &&
    observed_.fresh_until_ns > evaluation_time_ns;
  const bool authority_fresh =
    authority_session_.has_value() &&
    authority_session_->profile == config_.authority_profile &&
    authority_session_->valid_until_ns > evaluation_time_ns;
  return observation_fresh &&
         is_true(observed_.transport_connected) &&
         is_true(observed_.identity_verified) &&
         is_true(observed_.position_valid) &&
         is_true(observed_.robot_enabled) &&
         is_true(observed_.vendor_control_ready) &&
         is_true(observed_.hardware_component_active) &&
         is_true(observed_.trajectory_controller_active) &&
         is_false(observed_.fault_present) &&
         is_false(observed_.recovery_required) &&
         authority_fresh &&
         !pending_transaction_.has_value() &&
         observed_.adapter_blocking_reason_codes.empty();
}

LifecycleSnapshot Supervisor::snapshot(std::int64_t now_ns) const
{
  const std::int64_t evaluation_time_ns =
    std::max(now_ns, last_transition_time_ns_);
  LifecycleSnapshot result;
  result.robot_id = config_.robot_id;
  result.supervisor_instance_id = config_.supervisor_instance_id;
  result.authority_profile = config_.authority_profile;
  result.generation = snapshot_generation_;
  result.capability_generation = capability_generation_;
  result.capability_observation_generation =
    capability_observation_generation_;
  result.authority_session = authority_session_;
  result.observed = observed_;
  result.capabilities = capabilities_;
  result.pending_transaction = pending_transaction_;
  result.last_terminal_transaction = last_terminal_transaction_;

  result.observation_fresh =
    observed_.generation > 0 &&
    observed_.received_timestamp_ns <= evaluation_time_ns &&
    observed_.fresh_until_ns > evaluation_time_ns;
  result.authority_fresh =
    authority_session_.has_value() &&
    authority_session_->profile == config_.authority_profile &&
    authority_session_->valid_until_ns > evaluation_time_ns;
  result.recovery_required =
    is_true(observed_.fault_present) ||
    is_true(observed_.recovery_required);
  result.reconciliation_required =
    pending_transaction_.has_value() &&
    pending_transaction_->phase == TransactionPhase::kOutcomeUnknown;

  result.observe_ready =
    result.observation_fresh &&
    is_true(observed_.transport_connected) &&
    is_true(observed_.identity_verified) &&
    is_true(observed_.position_valid);
  result.controller_ready =
    result.observe_ready &&
    is_true(observed_.robot_enabled) &&
    is_true(observed_.vendor_control_ready) &&
    is_true(observed_.hardware_component_active) &&
    is_true(observed_.trajectory_controller_active) &&
    is_false(observed_.fault_present) &&
    is_false(observed_.recovery_required);
  result.motion_admitted = motion_admitted(evaluation_time_ns);

  if (!result.observation_fresh) {
    add_reason(result.admission_reason_codes, "observation_stale_or_missing");
  }
  if (!is_true(observed_.transport_connected)) {
    add_reason(result.admission_reason_codes, "transport_not_connected");
  }
  if (!is_true(observed_.identity_verified)) {
    add_reason(result.admission_reason_codes, "identity_not_verified");
  }
  if (!is_true(observed_.position_valid)) {
    add_reason(result.admission_reason_codes, "position_not_valid");
  }
  if (!is_true(observed_.robot_enabled)) {
    add_reason(result.admission_reason_codes, "robot_not_enabled");
  }
  if (!is_true(observed_.vendor_control_ready)) {
    add_reason(result.admission_reason_codes, "vendor_control_not_ready");
  }
  if (!is_true(observed_.hardware_component_active)) {
    add_reason(result.admission_reason_codes, "hardware_component_not_active");
  }
  if (!is_true(observed_.trajectory_controller_active)) {
    add_reason(result.admission_reason_codes, "trajectory_controller_not_active");
  }
  if (!is_false(observed_.fault_present)) {
    add_reason(result.admission_reason_codes, "fault_present_or_unknown");
  }
  if (!is_false(observed_.recovery_required)) {
    add_reason(result.admission_reason_codes, "recovery_required_or_unknown");
  }
  if (!result.authority_fresh) {
    add_reason(result.admission_reason_codes, "authority_session_stale_or_missing");
  }
  if (pending_transaction_.has_value()) {
    add_reason(
        result.admission_reason_codes,
        result.reconciliation_required ?
              "transaction_reconciliation_required" :
              "lifecycle_transaction_pending");
  }
  for (const auto & reason : observed_.adapter_blocking_reason_codes) {
    add_reason(result.admission_reason_codes, reason);
  }
  return result;
}

const CapabilityDescriptor * Supervisor::find_capability(
  std::string_view action_id) const
{
  const auto found = std::find_if(
      capabilities_.begin(), capabilities_.end(),
    [action_id](const CapabilityDescriptor & capability) {
      return capability.action_id == action_id;
      });
  return found == capabilities_.end() ? nullptr : &*found;
}

std::string Supervisor::request_fingerprint(
  const CommandRequest & request) const
{
  std::ostringstream stream;
  stream << request.robot_id << '\x1f'
         << request.request_id << '\x1f'
         << request.request_sequence << '\x1f'
         << request.action_id << '\x1f'
         << static_cast<int>(request.authority_profile) << '\x1f'
         << request.authority_session_id << '\x1f'
         << request.authority_session_generation << '\x1f'
         << request.principal_id << '\x1f'
         << request.expected_snapshot_generation << '\x1f'
         << request.confirmation_evidence;
  return stream.str();
}

CommandDecision Supervisor::reject(
  const CommandRequest & request,
  std::string reason,
  bool record_for_replay)
{
  CommandDecision decision;
  decision.disposition = CommandDisposition::kRejected;
  decision.reason = std::move(reason);
  decision.phase = TransactionPhase::kFailed;
  if (record_for_replay) {
    remember(
        request.request_sequence, request_fingerprint(request), decision);
  }
  return decision;
}

void Supervisor::remember(
  std::uint64_t request_sequence,
  std::string fingerprint,
  const CommandDecision & decision)
{
  highest_request_sequence_ = request_sequence;
  replay_.emplace(
      request_sequence,
      ReplayEntry{std::move(fingerprint), decision});
  replay_order_.push_back(request_sequence);
  while (replay_order_.size() > config_.replay_capacity) {
    replay_.erase(replay_order_.front());
    replay_order_.pop_front();
  }
}

void Supervisor::update_replay_decision(
  const TransactionRecord & transaction)
{
  const auto found = replay_.find(transaction.request_sequence);
  if (found == replay_.end()) {
    return;
  }
  found->second.decision.transaction_generation = transaction.generation;
  found->second.decision.phase = transaction.phase;
  found->second.decision.deadline_ns = transaction.deadline_ns;
  if (!transaction.result_reason.empty()) {
    found->second.decision.reason = transaction.result_reason;
  }
}

void Supervisor::handle_authority_loss(
  std::string reason, std::int64_t now_ns)
{
  if (!pending_transaction_.has_value()) {
    return;
  }
  if (pending_transaction_->phase == TransactionPhase::kAccepted) {
    finish_terminal(
      pending_transaction_->generation, pending_transaction_->request_id,
      TransactionPhase::kFailed, reason + "_before_execution", now_ns);
    return;
  }
  mark_pending_outcome_unknown(std::move(reason), now_ns);
}

void Supervisor::mark_pending_outcome_unknown(
  std::string reason, std::int64_t now_ns)
{
  if (!pending_transaction_.has_value()) {
    return;
  }
  pending_transaction_->phase = TransactionPhase::kOutcomeUnknown;
  pending_transaction_->terminal_at_ns = now_ns;
  pending_transaction_->result_reason = std::move(reason);
  update_replay_decision(*pending_transaction_);
  bump_generation();
}

bool Supervisor::finish_terminal(
  std::uint64_t transaction_generation,
  std::string_view request_id,
  TransactionPhase phase,
  std::string reason,
  std::int64_t now_ns)
{
  if (!pending_matches(transaction_generation, request_id) ||
    (phase != TransactionPhase::kSucceeded &&
    phase != TransactionPhase::kFailed))
  {
    return false;
  }
  pending_transaction_->phase = phase;
  pending_transaction_->terminal_at_ns = now_ns;
  pending_transaction_->result_reason = std::move(reason);
  update_replay_decision(*pending_transaction_);
  last_terminal_transaction_ = pending_transaction_;
  pending_transaction_.reset();
  bump_generation();
  return true;
}

bool Supervisor::pending_matches(
  std::uint64_t transaction_generation,
  std::string_view request_id) const
{
  return pending_transaction_.has_value() &&
         pending_transaction_->generation == transaction_generation &&
         pending_transaction_->request_id == request_id;
}

bool Supervisor::accept_time(std::int64_t now_ns)
{
  if (now_ns < last_transition_time_ns_) {
    return false;
  }
  last_transition_time_ns_ = now_ns;
  return true;
}

void Supervisor::bump_generation()
{
  if (snapshot_generation_ == std::numeric_limits<std::uint64_t>::max()) {
    throw std::overflow_error("lifecycle snapshot generation exhausted");
  }
  ++snapshot_generation_;
}

}  // namespace manyforge::robot_lifecycle
