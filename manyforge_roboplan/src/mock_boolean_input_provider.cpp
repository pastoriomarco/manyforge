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

#include <manyforge_roboplan/mock_boolean_input_provider.hpp>

#include <algorithm>
#include <limits>
#include <mutex>
#include <set>
#include <utility>

namespace manyforge::roboplan_backend {
namespace {
core::ErrorDTO invalid() {
  return {"invalid_request", "Mock boolean input configuration is invalid", {}};
}
} // namespace

struct MockBooleanInputProvider::State final {
  State(std::shared_ptr<core::IIoSignalIngress> sink, core::IClock &time,
        MockBooleanInputSource supplied,
        std::optional<std::uint64_t> generation)
      : ingress(std::move(sink)), clock(time),
        acquisition{std::move(supplied.source_id),
                    std::move(supplied.incarnation),
                    0,
                    0,
                    {}},
        accepting(!generation), driver_enabled(!generation),
        driver_generation(generation.value_or(0U)) {
    acquisition.updates.reserve(supplied.inputs.size());
    initial_values.reserve(supplied.inputs.size());
    for (auto &input : supplied.inputs) {
      initial_values.push_back(input.initial_value);
      acquisition.updates.push_back({std::move(input.descriptor.signal_id),
                                     core::IoSampleValidity::kValid,
                                     input.initial_value, std::nullopt});
    }
  }

  void Lose(core::IoIngressLossReason reason, core::TimePoint received_at_ns) {
    ingress->NotifyLoss({acquisition.source_id, acquisition.incarnation,
                         received_at_ns, ingress_sequence, reason,
                         std::nullopt});
  }

  std::shared_ptr<core::IIoSignalIngress> ingress;
  core::IClock &clock;
  core::IoSourceAcquisitionDTO acquisition;
  std::vector<bool> initial_values;
  mutable std::mutex mutex;
  bool accepting = true;
  bool driver_enabled = true;
  bool retired = false;
  std::uint64_t driver_generation = 0U;
  std::optional<std::uint64_t> resumed_sequence;
  std::optional<core::ErrorDTO> resumed_error;
  std::uint64_t ingress_sequence = 0;
  std::uint64_t enqueued_sequence = 0;
  bool initial_attempted = false;
  std::optional<std::uint64_t> initial_enqueued_sequence;
  bool rebound = false;
  std::optional<std::uint64_t> rebind_sequence;
  std::optional<core::ErrorDTO> rebind_error;

  core::Result<std::uint64_t> Publish(core::TimePoint received_at_ns) {
    using Reply = core::Result<std::uint64_t>;
    if (ingress_sequence == std::numeric_limits<std::uint64_t>::max()) {
      Lose(core::IoIngressLossReason::kSequenceExhausted, received_at_ns);
      accepting = false;
      return Reply::Err({"io_source_unavailable",
                         "Mock input source sequence exhausted",
                         {}});
    }
    acquisition.source_sequence = ++ingress_sequence;
    acquisition.trusted_received_at_ns = received_at_ns;
    if (!ingress->TryEnqueue(acquisition))
      return Reply::Err({"io_source_unavailable",
                         "Mock acquisition was refused by shared ingress",
                         {}});
    enqueued_sequence = ingress_sequence;
    return Reply::Ok(enqueued_sequence);
  }
};

core::Result<std::unique_ptr<MockBooleanInputProvider>>
MockBooleanInputProvider::Create(
    std::shared_ptr<core::IIoSignalIngress> ingress, core::IClock &clock,
    MockBooleanInputSource source,
    std::optional<std::uint64_t> prepared_driver_generation) {
  using Reply = core::Result<std::unique_ptr<MockBooleanInputProvider>>;
  if (!ingress || source.source_id.value().empty() ||
      source.incarnation.value().empty() || source.inputs.empty() ||
      (prepared_driver_generation && *prepared_driver_generation == 0U))
    return Reply::Err(invalid());
  std::set<core::IoSignalId> seen;
  for (const auto &input : source.inputs) {
    const auto &descriptor = input.descriptor;
    if (descriptor.signal_id.controllable_component_id.value().empty() ||
        descriptor.signal_id.name.empty() ||
        descriptor.source_id != source.source_id ||
        !seen.insert(descriptor.signal_id).second)
      return Reply::Err(invalid());
    if (descriptor.direction != core::IoSignalDirection::kInput ||
        descriptor.value_type != core::IoSignalValueType::kBoolean ||
        descriptor.update_mode != core::IoSignalUpdateMode::kChangeOnly ||
        descriptor.transition_capability !=
            core::IoTransitionCapability::kSampledOnly ||
        descriptor.freshness.mode != core::IoFreshnessMode::kProviderValidity ||
        descriptor.liveness != core::IoSignalLivenessMode::kInProcess)
      return Reply::Err(
          {"unsupported_capability",
           "Mock source requires event-driven boolean input validity",
           {}});
    if (descriptor.freshness.maximum_age_ns)
      return Reply::Err(invalid());
  }
  return Reply::Ok(
      std::unique_ptr<MockBooleanInputProvider>(new MockBooleanInputProvider(
          std::make_unique<State>(std::move(ingress), clock, std::move(source),
                                  prepared_driver_generation))));
}

MockBooleanInputProvider::MockBooleanInputProvider(std::unique_ptr<State> state)
    : state_(std::move(state)) {}
MockBooleanInputProvider::~MockBooleanInputProvider() { Shutdown(); }

core::Result<std::uint64_t> MockBooleanInputProvider::PublishInitial() {
  using Reply = core::Result<std::uint64_t>;
  std::lock_guard lock(state_->mutex);
  if (!state_->accepting)
    return Reply::Err({"io_source_unavailable",
                       "Mock boolean input provider is stopped",
                       {}});
  if (state_->initial_attempted) {
    if (state_->initial_enqueued_sequence)
      return Reply::Ok(*state_->initial_enqueued_sequence);
    return Reply::Err({"io_source_unavailable",
                       "Mock initial acquisition was not accepted",
                       {}});
  }
  state_->initial_attempted = true;
  const auto received_at_ns = state_->clock.now();
  const auto published = state_->Publish(received_at_ns);
  if (published.ok())
    state_->initial_enqueued_sequence = published.value();
  return published;
}

core::Result<MockBooleanInputSetReceipt>
MockBooleanInputProvider::SetValue(const core::IoSignalId &signal, bool value) {
  using Reply = core::Result<MockBooleanInputSetReceipt>;
  std::lock_guard lock(state_->mutex);
  if (state_->retired)
    return Reply::Err(
        {"io_source_unavailable", "Mock input source is retired", {}});
  const auto found = std::find_if(
      state_->acquisition.updates.begin(), state_->acquisition.updates.end(),
      [&](const auto &input) { return input.signal_id == signal; });
  if (found == state_->acquisition.updates.end())
    return Reply::Err(invalid());
  // Synthetic input acquisition linearizes at this model mutation, not at
  // request arrival. Concurrent manual/timer callers keep sequence/time order.
  const auto received_at_ns = state_->clock.now();
  found->value = value;
  if (!state_->accepting || !state_->initial_attempted)
    return Reply::Ok({value, std::nullopt, std::nullopt});
  const auto published = state_->Publish(received_at_ns);
  if (published.ok())
    return Reply::Ok({value, published.value(), std::nullopt});
  return Reply::Ok({value, std::nullopt, published.error()});
}

bool MockBooleanInputProvider::Pause(std::uint64_t generation) {
  std::lock_guard lock(state_->mutex);
  if (state_->retired || generation == 0U ||
      generation < state_->driver_generation)
    return false;
  if (generation == state_->driver_generation)
    return !state_->driver_enabled;
  state_->driver_generation = generation;
  state_->driver_enabled = false;
  if (state_->accepting) {
    state_->accepting = false;
    state_->Lose(core::IoIngressLossReason::kDisconnected, state_->clock.now());
  }
  return true;
}

core::Result<std::uint64_t>
MockBooleanInputProvider::Resume(std::uint64_t generation) {
  using Reply = core::Result<std::uint64_t>;
  std::lock_guard lock(state_->mutex);
  if (state_->retired || generation == 0U ||
      generation < state_->driver_generation ||
      (generation == state_->driver_generation && !state_->accepting))
    return Reply::Err({"io_source_unavailable",
                       "Mock driver Enable is stale or retired",
                       {}});
  if (generation == state_->driver_generation) {
    if (state_->resumed_sequence)
      return Reply::Ok(*state_->resumed_sequence);
    if (state_->resumed_error)
      return Reply::Err(*state_->resumed_error);
    return Reply::Err({"io_source_unavailable",
                       "Mock driver Enable has no acquisition receipt",
                       {}});
  }
  state_->driver_generation = generation;
  state_->accepting = true;
  state_->driver_enabled = true;
  state_->initial_attempted = true;
  state_->resumed_sequence.reset();
  state_->resumed_error.reset();
  const auto published = state_->Publish(state_->clock.now());
  if (published.ok()) {
    state_->resumed_sequence = published.value();
    if (!state_->initial_enqueued_sequence)
      state_->initial_enqueued_sequence = published.value();
  } else {
    state_->resumed_error = published.error();
  }
  return published;
}

MockBooleanInputStatus MockBooleanInputProvider::status() const {
  std::lock_guard lock(state_->mutex);
  return {state_->accepting, state_->ingress_sequence,
          state_->enqueued_sequence};
}

core::Result<std::optional<std::uint64_t>>
MockBooleanInputProvider::RebindIngress(
    std::shared_ptr<core::IIoSignalIngress> ingress,
    core::IoSourceIncarnation incarnation, std::uint64_t generation) {
  using Reply = core::Result<std::optional<std::uint64_t>>;
  std::lock_guard lock(state_->mutex);
  if (state_->retired || !ingress || incarnation.empty() || generation == 0U)
    return Reply::Err(invalid());
  if (incarnation == state_->acquisition.incarnation) {
    if (!state_->rebound || state_->ingress != ingress ||
        generation != state_->driver_generation)
      return Reply::Err(
          {"io_source_unavailable", "Mock source rebind is stale", {}});
    if (state_->rebind_error)
      return Reply::Err(*state_->rebind_error);
    return Reply::Ok(state_->rebind_sequence);
  }
  if (generation <= state_->driver_generation)
    return Reply::Err(
        {"io_source_unavailable", "Mock source rebind is stale", {}});
  if (state_->accepting)
    state_->Lose(core::IoIngressLossReason::kDisconnected, state_->clock.now());
  state_->ingress = std::move(ingress);
  state_->accepting = state_->driver_enabled;
  state_->acquisition.incarnation = std::move(incarnation);
  state_->acquisition.source_sequence = 0U;
  state_->acquisition.trusted_received_at_ns = 0;
  state_->ingress_sequence = 0U;
  state_->enqueued_sequence = 0U;
  state_->initial_attempted = false;
  state_->initial_enqueued_sequence.reset();
  state_->resumed_sequence.reset();
  state_->resumed_error.reset();
  state_->rebind_sequence.reset();
  state_->rebind_error.reset();
  state_->driver_generation = generation;
  state_->rebound = true;
  const auto acquired_at = state_->clock.now();
  for (std::size_t index = 0U; index < state_->acquisition.updates.size();
       ++index)
    state_->acquisition.updates[index].value = state_->initial_values[index];
  if (!state_->accepting)
    return Reply::Ok(std::nullopt);
  state_->initial_attempted = true;
  const auto published = state_->Publish(acquired_at);
  if (!published.ok()) {
    state_->rebind_error = published.error();
    state_->resumed_error = published.error();
    return Reply::Err(published.error());
  }
  state_->initial_enqueued_sequence = published.value();
  state_->rebind_sequence = published.value();
  state_->resumed_sequence = published.value();
  return Reply::Ok(state_->rebind_sequence);
}

void MockBooleanInputProvider::Shutdown() {
  std::lock_guard lock(state_->mutex);
  state_->retired = true;
  state_->driver_enabled = false;
  if (!state_->accepting)
    return;
  const auto received_at_ns = state_->clock.now();
  state_->accepting = false;
  state_->Lose(core::IoIngressLossReason::kDisconnected, received_at_ns);
}

} // namespace manyforge::roboplan_backend
