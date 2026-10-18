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

#include <manyforge_core/ports/iio_signal_ingress.hpp>
#include <memory>
#include <optional>
#include <vector>

namespace manyforge::roboplan_backend {

struct MockBooleanInput final {
  core::IoSignalDescriptorDTO descriptor;
  bool initial_value = false;
};

/// Resolved sealed mock input configuration. The existing host installs and
/// exclusively assigns the exact source/incarnation before creating ingress.
struct MockBooleanInputSource final {
  core::InterruptSourceId source_id;
  core::IoSourceIncarnation incarnation;
  std::vector<MockBooleanInput> inputs;
};

struct MockBooleanInputStatus final {
  bool accepting;
  std::uint64_t last_ingress_source_sequence;
  std::uint64_t last_enqueued_source_sequence;
};

struct MockBooleanInputSetReceipt final {
  bool model_value;
  /// Queue acceptance only; the host separately verifies actual Core delivery.
  std::optional<std::uint64_t> enqueued_source_sequence;
  std::optional<core::ErrorDTO> publication_error;
};

/// Event-driven authoritative mock input model; no timer or output API. The
/// host installs the initial snapshot and invokes explicit SetValue events;
/// delayed auto-answers/manual cancellation belong to existing host
/// registration ownership. All input values form one atomic source acquisition.
/// Core alone exposes delivered observations; model changes can outlive failed
/// ingress. Restart constructs a provider; the original Apply owner rebinds a
/// retained driver and resets its authored initial model exactly once. Same
/// IClock as Core; quiesce host calls before releasing clock/provider.
class MockBooleanInputProvider final {
public:
  static core::Result<std::unique_ptr<MockBooleanInputProvider>> Create(
      std::shared_ptr<core::IIoSignalIngress> ingress, core::IClock &clock,
      MockBooleanInputSource source,
      std::optional<std::uint64_t> prepared_driver_generation = std::nullopt);
  ~MockBooleanInputProvider();
  MockBooleanInputProvider(const MockBooleanInputProvider &) = delete;
  MockBooleanInputProvider &
  operator=(const MockBooleanInputProvider &) = delete;

  /// One idempotent initial acquisition; a refused baseline is not resent.
  core::Result<std::uint64_t> PublishInitial();
  /// An explicit model set, including an equal value, emits a full-source
  /// snapshot stamped at serialized model mutation, before enqueue. Ingress
  /// refusal leaves the model changed and reports publication_error. Paused
  /// sources acknowledge model mutation without claiming queue/delivery. A
  /// later explicit event observes that model again with a new receiver
  /// timestamp and sequence.
  core::Result<MockBooleanInputSetReceipt>
  SetValue(const core::IoSignalId &signal, bool value);
  MockBooleanInputStatus status() const;
  bool Pause(std::uint64_t driver_observation_generation);
  /// One preserved full-source acquisition per actual owner-issued Enable;
  /// exact replay reads its retained result, never resubmits or reseeds.
  core::Result<std::uint64_t>
  Resume(std::uint64_t driver_observation_generation);
  /// Existing Apply/domain owner only, with its new source incarnation. Keeps
  /// the actual driver gate; restores initial values once and emits a real
  /// baseline only if enabled. Exact replay reads the retained enqueue result.
  core::Result<std::optional<std::uint64_t>>
  RebindIngress(std::shared_ptr<core::IIoSignalIngress> ingress,
                core::IoSourceIncarnation incarnation,
                std::uint64_t driver_observation_generation);
  /// Fences model events and notifies an ordered source disconnection once.
  void Shutdown();

private:
  struct State;
  explicit MockBooleanInputProvider(std::unique_ptr<State> state);
  std::unique_ptr<State> state_;
};

} // namespace manyforge::roboplan_backend
