// Copyright 2026 Marco Pastorio
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

#include <manyforge_roboplan/mock_boolean_output_provider.hpp>

#include <algorithm>
#include <iostream>
#include <manyforge_core/types/projection_common.hpp>
#include <mutex>
#include <set>
#include <utility>

namespace manyforge::roboplan_backend {
namespace {

template <class T>
core::Result<T> reject(std::string reason, std::string message) {
  return core::Result<T>::Err({"io_output_rejected",
                               std::move(message),
                               {{"reason", std::move(reason)}}});
}

bool portable_channel(std::string_view value) {
  if (value.empty() || value.size() > 128U)
    return false;
  const auto alphanumeric = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
  };
  return alphanumeric(value.front()) &&
         std::all_of(value.begin(), value.end(), [&](char c) {
           return alphanumeric(c) || c == '.' || c == '_' || c == '-';
         });
}

bool valid_identity(const core::IoOutputCommandIdentityDTO &identity) {
  const auto &command = identity.command;
  return !command.operation_id.empty() &&
         command.operation_revision.nonzero() &&
         !command.participant_id.empty() &&
         !command.command_id.empty() &&
         command.command_generation.nonzero() &&
         identity.component_lifecycle_generation.nonzero() &&
         core::IsExactLowercaseSha256Digest(
             identity.prepared_contract_digest.value()) &&
         identity.provider.locally_valid();
}

} // namespace

struct MockBooleanOutputProvider::State final {
  struct Gate final {
    core::ResolvedLifecycleProviderOccurrenceDTO provider;
    std::uint64_t generation;
    std::weak_ptr<const void> actual_driver_owner;
    bool accepting = false;
    bool terminal = false;
  };
  struct Alias final {
    core::IoSignalId signal_id;
    std::size_t gate;
  };
  struct Record final {
    core::IoOutputWriteRequestDTO request;
    core::IoOutputObservationDTO observation;
  };

  struct AliasTable final {
    std::vector<Gate> gates;
    std::vector<Alias> aliases;
  };
  static core::Result<AliasTable>
  PrepareAliases(const std::vector<MockBooleanOutputAlias> &configuration) {
    if (configuration.empty())
      return reject<AliasTable>("mock_output_alias_invalid",
                                "Output alias table is empty");
    AliasTable table;
    std::set<core::IoSignalId> signals;
    for (const auto &alias : configuration) {
      if (alias.signal_id.controllable_component_id.empty() ||
          alias.signal_id.name.empty() || !alias.provider.locally_valid() ||
          !alias.prepared_driver_generation ||
          alias.actual_driver_owner.expired() ||
          !signals.insert(alias.signal_id).second)
        return reject<AliasTable>(
            "mock_output_alias_invalid",
            "Output alias is missing, ambiguous or has no live actual owner");
      const auto gate = std::find_if(
          table.gates.begin(), table.gates.end(),
          [&](const auto &row) { return row.provider == alias.provider; });
      const auto index = static_cast<std::size_t>(gate - table.gates.begin());
      if (gate == table.gates.end())
        table.gates.push_back({alias.provider, alias.prepared_driver_generation,
                               alias.actual_driver_owner, false, false});
      else if (gate->generation != alias.prepared_driver_generation ||
               !SameIoOutputDriverOwner(gate->actual_driver_owner,
                                        alias.actual_driver_owner))
        return reject<AliasTable>(
            "mock_output_generation_mismatch",
            "Aliases disagree on their actual driver lifetime or observation");
      table.aliases.push_back({alias.signal_id, index});
    }
    std::sort(
        table.aliases.begin(), table.aliases.end(),
        [](const auto &a, const auto &b) { return a.signal_id < b.signal_id; });
    return core::Result<AliasTable>::Ok(std::move(table));
  }
  bool Matches(const std::vector<IoOutputAliasIdentity> &expected) const {
    if (aliases.size() != expected.size())
      return false;
    for (std::size_t i = 0; i < aliases.size(); ++i) {
      const auto &gate = gates[aliases[i].gate];
      if (aliases[i].signal_id != expected[i].signal_id ||
          gate.provider != expected[i].provider ||
          !SameIoOutputDriverOwner(gate.actual_driver_owner,
                                   expected[i].actual_driver_owner))
        return false;
    }
    return true;
  }
  bool Matches(const AliasTable &candidate) const {
    if (aliases.size() != candidate.aliases.size())
      return false;
    for (std::size_t i = 0; i < aliases.size(); ++i) {
      const auto &gate = gates[aliases[i].gate];
      const auto &other = candidate.gates[candidate.aliases[i].gate];
      if (aliases[i].signal_id != candidate.aliases[i].signal_id ||
          gate.provider != other.provider ||
          !SameIoOutputDriverOwner(gate.actual_driver_owner,
                                   other.actual_driver_owner))
        return false;
    }
    return true;
  }
  State(core::IClock &clock,
        core::ActiveBundlePublicationGeneration publication)
      : publication(publication), clock(clock) {}
  core::ActiveBundlePublicationGeneration publication;
  core::IClock &clock;
  mutable std::mutex mutex;
  std::vector<Gate> gates;
  std::vector<Alias> aliases;
  std::optional<bool> model;
  std::optional<Record> record;
  std::function<void(bool, core::TimePoint)> model_observer;
  bool retired = false;
};

core::Result<std::unique_ptr<MockBooleanOutputProvider>>
MockBooleanOutputProvider::Create(core::IClock &clock,
                                  MockBooleanOutputChannel channel) {
  using Reply = core::Result<std::unique_ptr<MockBooleanOutputProvider>>;
  if (channel.resource_id.empty() || !portable_channel(channel.channel) ||
      channel.aliases.empty() ||
      !channel.active_publication_generation.nonzero())
    return reject<std::unique_ptr<MockBooleanOutputProvider>>(
        "mock_output_configuration_invalid",
        "Mock output requires one configured channel");
  auto aliases = State::PrepareAliases(channel.aliases);
  if (!aliases.ok())
    return Reply::Err(aliases.error());
  auto state =
      std::make_unique<State>(clock, channel.active_publication_generation);
  state->gates = std::move(aliases.value().gates);
  state->aliases = std::move(aliases.value().aliases);
  return Reply::Ok(std::unique_ptr<MockBooleanOutputProvider>(
      new MockBooleanOutputProvider(std::move(state))));
}

MockBooleanOutputProvider::MockBooleanOutputProvider(
    std::unique_ptr<State> state)
    : state_(std::move(state)) {}
MockBooleanOutputProvider::~MockBooleanOutputProvider() = default;

core::IoOutputEvidence
MockBooleanOutputProvider::StrongestOutputEvidence() const noexcept {
  return core::IoOutputEvidence::kSourceAccepted;
}

core::Result<core::IoOutputObservationDTO>
MockBooleanOutputProvider::SubmitOutput(
    const core::IoOutputWriteRequestDTO &request) {
  using Reply = core::Result<core::IoOutputObservationDTO>;
  std::shared_ptr<const void> driver_pin;
  std::lock_guard lock(state_->mutex);
  if (state_->record) {
    if (state_->record->request == request)
      return Reply::Ok(state_->record->observation);
    return reject<core::IoOutputObservationDTO>(
        state_->record->request.identity == request.identity
            ? "io_output_request_conflict"
            : "io_output_correlation_busy",
        "Existing command owner must retire its exact output correlation");
  }
  if (!valid_identity(request.identity))
    return reject<core::IoOutputObservationDTO>(
        "io_output_identity_invalid",
        "Output requires its actual committed identity");
  const auto alias = std::find_if(
      state_->aliases.begin(), state_->aliases.end(),
      [&](const auto &row) { return row.signal_id == request.signal_id; });
  if (alias == state_->aliases.end() ||
      state_->gates[alias->gate].provider != request.identity.provider)
    return reject<core::IoOutputObservationDTO>(
        "io_output_provider_mismatch",
        "Output alias does not name its configured occurrence");
  driver_pin = state_->gates[alias->gate].actual_driver_owner.lock();
  std::optional<core::ErrorDTO> refusal;
  if (state_->retired || !driver_pin || !state_->gates[alias->gate].accepting) {
    refusal =
        core::ErrorDTO{"io_output_not_submitted",
                       "Mock output provider occurrence is disabled or retired",
                       {{"reason", "io_output_driver_disabled"}}};
  }
  // Allocate/copy the whole retained correlation before checking the actual
  // acceptance instant or changing the model. A lost return after the write
  // leaves the exact accepted fact queryable; retired IDs need no second
  // ledger.
  state_->record.emplace(
      State::Record{request,
                    {request.identity, core::IoOutputEvidence::kNotSubmitted, 0,
                     std::move(refusal)}});
  const auto now = state_->clock.now();
  state_->record->observation.trusted_received_at_ns = now;
  if (state_->record->observation.error)
    return Reply::Ok(state_->record->observation);
  if (request.absolute_deadline_ns) {
    state_->record->observation.error = core::ErrorDTO{
        "io_output_not_submitted",
        "Immediate Mock output does not accept a timed completion policy",
        {{"reason", "io_output_completion_policy_mismatch"}}};
    return Reply::Ok(state_->record->observation);
  }
  state_->model = request.value;
  state_->record->observation.evidence =
      core::IoOutputEvidence::kSourceAccepted;
  if (state_->model_observer) {
    try {
      state_->model_observer(request.value, now);
    } catch (...) {
      // Output acceptance is already settled; a failed synthetic echo must
      // not be rewritten into a fabricated no-effect output result.
      std::cerr << "Mock I/O echo could not be scheduled\n";
    }
  }
  return Reply::Ok(state_->record->observation);
}

core::Result<core::IoOutputObservationDTO>
MockBooleanOutputProvider::ObserveOutputResult(
    const core::IoOutputCommandIdentityDTO &identity) const {
  std::lock_guard lock(state_->mutex);
  if (!state_->record || state_->record->request.identity != identity)
    return reject<core::IoOutputObservationDTO>(
        "io_output_correlation_unavailable",
        "No retained evidence for this exact output command");
  return core::Result<core::IoOutputObservationDTO>::Ok(
      state_->record->observation);
}

core::Result<void> MockBooleanOutputProvider::RetireOutput(
    const core::IoOutputCommandIdentityDTO &identity) {
  std::lock_guard lock(state_->mutex);
  if (state_->record && state_->record->request.identity != identity)
    return reject<void>("io_output_correlation_mismatch",
                        "Retirement cannot clear another command");
  state_->record.reset();
  return core::Result<void>::Ok();
}

core::Result<void> MockBooleanOutputProvider::Pause(
    const core::ResolvedLifecycleProviderOccurrenceDTO &provider,
    std::uint64_t generation,
    const std::weak_ptr<const void> &actual_driver_owner) {
  const auto driver_pin = actual_driver_owner.lock();
  std::lock_guard lock(state_->mutex);
  const auto gate = std::find_if(
      state_->gates.begin(), state_->gates.end(), [&](const auto &row) {
        return row.provider == provider &&
               SameIoOutputDriverOwner(row.actual_driver_owner,
                                       actual_driver_owner);
      });
  if (!driver_pin || state_->retired || gate == state_->gates.end() ||
      gate->terminal || generation == 0U || generation < gate->generation ||
      (generation == gate->generation && gate->accepting))
    return reject<void>("io_output_driver_generation_stale",
                        "Mock output Disable is stale or retired");
  gate->generation = generation;
  gate->accepting = false;
  return core::Result<void>::Ok();
}

core::Result<void> MockBooleanOutputProvider::Resume(
    const core::ResolvedLifecycleProviderOccurrenceDTO &provider,
    std::uint64_t generation,
    const std::weak_ptr<const void> &actual_driver_owner) {
  const auto driver_pin = actual_driver_owner.lock();
  std::lock_guard lock(state_->mutex);
  const auto gate = std::find_if(
      state_->gates.begin(), state_->gates.end(), [&](const auto &row) {
        return row.provider == provider &&
               SameIoOutputDriverOwner(row.actual_driver_owner,
                                       actual_driver_owner);
      });
  if (!driver_pin || state_->retired || gate == state_->gates.end() ||
      gate->terminal || generation == 0U || generation < gate->generation ||
      (generation == gate->generation && !gate->accepting))
    return reject<void>("io_output_driver_generation_stale",
                        "Mock output Enable is stale or retired");
  gate->generation = generation;
  gate->accepting = true;
  return core::Result<void>::Ok();
}

core::Result<void> MockBooleanOutputProvider::ShutdownOccurrence(
    const core::ResolvedLifecycleProviderOccurrenceDTO &provider,
    const std::weak_ptr<const void> &actual_driver_owner) {
  std::lock_guard lock(state_->mutex);
  const auto gate = std::find_if(
      state_->gates.begin(), state_->gates.end(), [&](const auto &row) {
        return row.provider == provider &&
               SameIoOutputDriverOwner(row.actual_driver_owner,
                                       actual_driver_owner);
      });
  if (gate == state_->gates.end())
    return reject<void>(
        "io_output_provider_mismatch",
        "Terminal fence does not name a configured output occurrence");
  gate->accepting = false;
  gate->terminal = true;
  return core::Result<void>::Ok();
}

IoOutputAliasState MockBooleanOutputProvider::ReadAliasState() const {
  std::lock_guard lock(state_->mutex);
  std::vector<IoOutputAliasIdentity> aliases;
  aliases.reserve(state_->aliases.size());
  for (const auto &alias : state_->aliases) {
    const auto &gate = state_->gates[alias.gate];
    aliases.push_back(
        {alias.signal_id, gate.provider, gate.actual_driver_owner});
  }
  return {state_->publication, std::move(aliases)};
}

core::Result<void> MockBooleanOutputProvider::RebindAliases(
    const std::vector<IoOutputAliasIdentity> &expected_current_aliases,
    const std::vector<MockBooleanOutputAlias> &candidate_aliases,
    core::ActiveBundlePublicationGeneration publication) {
  if (!publication.nonzero())
    return reject<void>(
        "io_output_publication_invalid",
        "Output alias replacement requires the actual publication");
  auto prepared = State::PrepareAliases(candidate_aliases);
  if (!prepared.ok())
    return core::Result<void>::Err(prepared.error());
  auto &table = prepared.value();
  // Release any final actual owner only after the channel mutex is unlocked;
  // its destructor may perform its exact occurrence fence on this channel.
  std::vector<std::shared_ptr<const void>> driver_pins;
  driver_pins.reserve(table.gates.size());
  for (const auto &gate : table.gates) {
    auto owner = gate.actual_driver_owner.lock();
    if (!owner)
      return reject<void>("io_output_driver_retired",
                          "Candidate actual driver is retired");
    driver_pins.push_back(std::move(owner));
  }
  std::lock_guard lock(state_->mutex);
  if (state_->retired || publication < state_->publication)
    return reject<void>("io_output_alias_rebind_stale",
                        "Output channel or publication is retired");
  if (publication == state_->publication) {
    if (state_->Matches(table))
      return core::Result<void>::Ok();
    return reject<void>("io_output_publication_conflict",
                        "The same publication cannot replace output aliases");
  }
  if (!state_->Matches(expected_current_aliases))
    return reject<void>("io_output_alias_predecessor_mismatch",
                        "The complete actual predecessor alias table changed");
  for (auto &gate : table.gates) {
    const auto retained = std::find_if(
        state_->gates.begin(), state_->gates.end(), [&](const auto &old) {
          return old.provider == gate.provider &&
                 SameIoOutputDriverOwner(old.actual_driver_owner,
                                         gate.actual_driver_owner);
        });
    if (retained != state_->gates.end())
      gate = *retained;
  }
  state_->gates.swap(table.gates);
  state_->aliases.swap(table.aliases);
  state_->publication = publication;
  state_->model.reset();
  return core::Result<void>::Ok();
}

void MockBooleanOutputProvider::Shutdown() {
  std::lock_guard lock(state_->mutex);
  state_->retired = true;
  for (auto &gate : state_->gates)
    gate.accepting = false;
}

void MockBooleanOutputProvider::SetModelObserver(
    std::function<void(bool, core::TimePoint)> observer) {
  std::lock_guard lock(state_->mutex);
  state_->model_observer = std::move(observer);
}

std::unique_lock<std::mutex>
MockBooleanOutputProvider::AcquireModelDelivery() const {
  return std::unique_lock<std::mutex>(state_->mutex);
}

std::optional<bool> MockBooleanOutputProvider::model_value() const {
  std::lock_guard lock(state_->mutex);
  return state_->model;
}

} // namespace manyforge::roboplan_backend
