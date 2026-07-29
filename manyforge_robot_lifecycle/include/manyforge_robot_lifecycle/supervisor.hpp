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

#pragma once

#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <manyforge_robot_lifecycle/contract.hpp>

namespace manyforge::robot_lifecycle
{

class Supervisor final {
public:
  explicit Supervisor(SupervisorConfig config);

  Supervisor(const Supervisor &) = delete;
  Supervisor & operator=(const Supervisor &) = delete;

  bool publish_observation(const ObservedFacets & observation);
  bool renew_observation_lease(
    std::uint64_t observation_generation,
    std::int64_t source_timestamp_ns,
    std::int64_t received_timestamp_ns,
    std::int64_t fresh_until_ns);
  bool replace_capabilities(
    std::uint64_t capability_generation,
    std::uint64_t observation_generation,
    std::vector<CapabilityDescriptor> capabilities);

  bool establish_authority_session(
    const AuthoritySession & session, std::int64_t now_ns);
  bool clear_authority_session(
    std::string_view session_id,
    std::uint64_t session_generation,
    std::int64_t now_ns);

  CommandDecision submit(
    const CommandRequest & request, std::int64_t now_ns);
  bool mark_executing(
    std::uint64_t transaction_generation,
    std::string_view request_id,
    std::int64_t now_ns);
  bool mark_verifying(
    std::uint64_t transaction_generation,
    std::string_view request_id,
    std::int64_t now_ns);
  bool finish_success(
    std::uint64_t transaction_generation,
    std::string_view request_id,
    std::string reason,
    std::int64_t now_ns);
  bool finish_failure(
    std::uint64_t transaction_generation,
    std::string_view request_id,
    std::string reason,
    std::int64_t now_ns);
  bool mark_outcome_unknown(
    std::uint64_t transaction_generation,
    std::string_view request_id,
    std::string reason,
    std::int64_t now_ns);
  bool resolve_uncertain(
    std::uint64_t transaction_generation,
    std::string_view request_id,
    bool postcondition_satisfied,
    std::string reason,
    std::int64_t now_ns);

  bool advance_time(std::int64_t now_ns);
  [[nodiscard]] bool motion_admitted(std::int64_t now_ns) const noexcept;
  [[nodiscard]] LifecycleSnapshot snapshot(std::int64_t now_ns) const;

private:
  struct ReplayEntry
  {
    std::string fingerprint;
    CommandDecision decision;
  };

  [[nodiscard]] const CapabilityDescriptor * find_capability(
    std::string_view action_id) const;
  [[nodiscard]] std::string request_fingerprint(
    const CommandRequest & request) const;
  [[nodiscard]] CommandDecision reject(
    const CommandRequest & request,
    std::string reason,
    bool record_for_replay);
  void remember(
    std::uint64_t request_sequence,
    std::string fingerprint,
    const CommandDecision & decision);
  void update_replay_decision(const TransactionRecord & transaction);
  void handle_authority_loss(
    std::string reason, std::int64_t now_ns);
  void mark_pending_outcome_unknown(
    std::string reason, std::int64_t now_ns);
  bool finish_terminal(
    std::uint64_t transaction_generation,
    std::string_view request_id,
    TransactionPhase phase,
    std::string reason,
    std::int64_t now_ns);
  [[nodiscard]] bool pending_matches(
    std::uint64_t transaction_generation,
    std::string_view request_id) const;
  bool accept_time(std::int64_t now_ns);
  void bump_generation();

  SupervisorConfig config_;
  std::uint64_t snapshot_generation_ = 1;
  std::uint64_t capability_generation_ = 0;
  std::uint64_t capability_observation_generation_ = 0;
  std::uint64_t next_transaction_generation_ = 1;
  std::uint64_t highest_authority_session_generation_ = 0;
  std::uint64_t highest_request_sequence_ = 0;
  ObservedFacets observed_;
  std::vector<CapabilityDescriptor> capabilities_;
  std::optional<AuthoritySession> authority_session_;
  std::optional<TransactionRecord> pending_transaction_;
  std::optional<TransactionRecord> last_terminal_transaction_;
  std::unordered_map<std::uint64_t, ReplayEntry> replay_;
  std::deque<std::uint64_t> replay_order_;
  std::int64_t last_transition_time_ns_ =
    std::numeric_limits<std::int64_t>::min();
};

}  // namespace manyforge::robot_lifecycle
