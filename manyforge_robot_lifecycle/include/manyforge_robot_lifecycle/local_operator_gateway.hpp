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

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <manyforge_robot_lifecycle/control_port.hpp>

namespace manyforge::robot_lifecycle
{

enum class CallerAssurance
{
  kUnverified,
  kAuthenticated,
};

/// Server-side command-source context.
///
/// `kUnverified` is an honest commissioning/private-deployment assertion: it
/// identifies the software ingress for audit but does not claim a human was
/// authenticated. A future credential adapter may strengthen the same
/// protocol to `kAuthenticated` without changing lifecycle transactions.
struct CallerContext
{
  std::string command_source;
  std::string caller_context_id;
  CallerAssurance assurance = CallerAssurance::kUnverified;
  std::int64_t asserted_at_ns = 0;
  std::int64_t valid_until_ns = 0;
};

struct LocalOperatorGatewayConfig
{
  std::string robot_id;
  std::int64_t maximum_authority_lease_ns = 300'000'000'000;
  std::size_t audit_capacity = 256;
  CallerAssurance minimum_assurance = CallerAssurance::kAuthenticated;
};

struct LocalAuthoritySessionIntent
{
  std::string session_id;
  std::uint64_t generation = 0;
  std::int64_t valid_until_ns = 0;
  std::string component_epoch;
};

/// Caller-controlled command fields. Robot, profile, session principal, and
/// authenticated identity are deliberately absent and are bound by the
/// gateway.
struct LocalLifecycleCommandIntent
{
  std::string request_id;
  std::uint64_t request_sequence = 0;
  std::string action_id;
  std::string authority_session_id;
  std::uint64_t authority_session_generation = 0;
  std::uint64_t expected_snapshot_generation = 0;
  std::string confirmation_evidence;
};

struct GatewayMutationResult
{
  bool caller_context_valid = false;
  bool accepted = false;
  std::string reason;
  std::optional<CommandDecision> command_decision;
};

struct GatewaySnapshotResult
{
  bool caller_context_valid = false;
  std::string reason;
  std::optional<LifecycleSnapshot> snapshot;
};

struct GatewayAuditRecord
{
  std::uint64_t sequence = 0;
  std::int64_t occurred_at_ns = 0;
  std::string command_source;
  std::string caller_context_id;
  CallerAssurance caller_assurance = CallerAssurance::kUnverified;
  std::string operation;
  std::string target_id;
  bool caller_context_valid = false;
  bool accepted = false;
  std::string reason;
};

/// Serialized headless local-operator boundary.
///
/// This class authenticates no credentials itself. It consumes a short-lived
/// server-side caller context, enforces the configured assurance floor, binds
/// the command source to lifecycle requests, and retains a bounded audit
/// trail. It owns no robot or transport.
class LocalOperatorGateway final
{
public:
  LocalOperatorGateway(
    LocalOperatorGatewayConfig config,
    LifecycleControlPort & control_port);

  LocalOperatorGateway(const LocalOperatorGateway &) = delete;
  LocalOperatorGateway & operator=(const LocalOperatorGateway &) = delete;

  [[nodiscard]] GatewaySnapshotResult snapshot(
    const CallerContext & caller,
    std::int64_t now_ns);
  GatewayMutationResult open_session(
    const CallerContext & caller,
    const LocalAuthoritySessionIntent & intent,
    std::int64_t now_ns);
  GatewayMutationResult close_session(
    const CallerContext & caller,
    std::string_view session_id,
    std::uint64_t session_generation,
    std::int64_t now_ns);
  GatewayMutationResult submit(
    const CallerContext & caller,
    const LocalLifecycleCommandIntent & intent,
    std::int64_t now_ns);
  GatewayMutationResult reconcile(
    const CallerContext & caller,
    std::uint64_t transaction_generation,
    std::string_view request_id,
    std::string reason,
    std::int64_t now_ns);

  [[nodiscard]] std::vector<GatewayAuditRecord> audit_snapshot() const;

private:
  struct ActiveSessionBinding
  {
    std::string session_id;
    std::uint64_t session_generation = 0;
    std::string command_source;
    std::string caller_context_id;
    std::string component_epoch;
  };

  [[nodiscard]] bool validate_caller(
    const CallerContext & caller,
    std::int64_t now_ns,
    std::string & reason,
    bool & caller_context_valid) const;
  [[nodiscard]] bool caller_owns_session(
    const CallerContext & caller,
    const LifecycleSnapshot & snapshot,
    std::string_view session_id,
    std::uint64_t session_generation) const;
  void record(
    const CallerContext & caller,
    std::string operation,
    std::string target_id,
    const GatewayMutationResult & result,
    std::int64_t now_ns);

  LocalOperatorGatewayConfig config_;
  LifecycleControlPort & control_port_;
  mutable std::mutex mutex_;
  std::optional<ActiveSessionBinding> active_session_binding_;
  std::deque<GatewayAuditRecord> audit_;
  std::uint64_t next_audit_sequence_ = 1;
};

}  // namespace manyforge::robot_lifecycle
