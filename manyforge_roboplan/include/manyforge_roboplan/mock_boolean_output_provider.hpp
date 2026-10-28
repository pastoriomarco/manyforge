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

#pragma once

#include <manyforge_core/ports/iio_output_provider.hpp>
#include <manyforge_roboplan/io_output_alias_identity.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace manyforge::roboplan_backend {

/// Exact sealed host alias of one physical synthetic output channel. Driver
/// generations are supplied by the existing lifecycle observation owner.
struct MockBooleanOutputAlias final {
  core::IoSignalId signal_id;
  core::ResolvedLifecycleProviderOccurrenceDTO provider;
  std::uint64_t prepared_driver_generation;
  std::weak_ptr<const void> actual_driver_owner;
};

struct MockBooleanOutputChannel final {
  core::ResourceId resource_id;
  std::string channel;
  std::vector<MockBooleanOutputAlias> aliases;
  core::ActiveBundlePublicationGeneration active_publication_generation;
};

/// One actual model/provider per canonical host channel/resource, shared by all
/// of its portable aliases. Host composition must group aliases before Create,
/// never instantiate independent models with equal resource IDs. Occurrence
/// gates remain separate so one domain's Enable cannot enable another alias.
///
/// Model acceptance is synthetic output acceptance only. A host observer may
/// use it for configured Mock input echo; the leaf owns no ingress, readback,
/// timer or thread. Preserve this provider
/// across Disable/Enable. Each actual new Apply resets the model unwritten
/// under the channel mutex; replay of that publication cannot reset it again.
/// Immediate model acceptance requires no deadline. The existing Kernel/session
/// owns admission, CCLC generation, policy and post-retirement replay fencing.
/// This leaf retains one correlation record.
/// Quiesce host calls before releasing this provider or its injected clock.
class MockBooleanOutputProvider final : public core::IIoOutputProvider {
public:
  [[nodiscard]] static core::Result<std::unique_ptr<MockBooleanOutputProvider>>
  Create(core::IClock &clock, MockBooleanOutputChannel channel);
  ~MockBooleanOutputProvider() override;
  MockBooleanOutputProvider(const MockBooleanOutputProvider &) = delete;
  MockBooleanOutputProvider &
  operator=(const MockBooleanOutputProvider &) = delete;

  core::IoOutputEvidence StrongestOutputEvidence() const noexcept override;
  core::Result<core::IoOutputObservationDTO>
  SubmitOutput(const core::IoOutputWriteRequestDTO &request) override;
  core::Result<core::IoOutputObservationDTO> ObserveOutputResult(
      const core::IoOutputCommandIdentityDTO &identity) const override;
  core::Result<void>
  RetireOutput(const core::IoOutputCommandIdentityDTO &identity) override;

  /// Called only under the existing host delivery/domain lifecycle lock after
  /// full expected-observation checks. These are transport fences, not another
  /// lifecycle decision. Never derive CCLC generation from this driver counter.
  core::Result<void>
  Pause(const core::ResolvedLifecycleProviderOccurrenceDTO &provider,
        std::uint64_t driver_observation_generation,
        const std::weak_ptr<const void> &actual_driver_owner);
  core::Result<void>
  Resume(const core::ResolvedLifecycleProviderOccurrenceDTO &provider,
         std::uint64_t driver_observation_generation,
         const std::weak_ptr<const void> &actual_driver_owner);
  /// Final local owner teardown, independent of counter exhaustion. This exact
  /// occurrence can never resume; other aliases and command evidence survive.
  core::Result<void> ShutdownOccurrence(
      const core::ResolvedLifecycleProviderOccurrenceDTO &provider,
      const std::weak_ptr<const void> &actual_driver_owner);
  /// Current canonical table for the trusted installer to join to its exact
  /// sealed predecessor/original candidate and held parent Apply proof.
  [[nodiscard]] IoOutputAliasState ReadAliasState() const;
  /// Trusted installer only, under the existing exact Apply pin. Atomically
  /// replaces the complete alias table, preserving current retained gates and
  /// pending correlation. New actual owners start paused. Exact publication
  /// replay is a no-op; predecessor comparison rejects stale metadata.
  core::Result<void> RebindAliases(
      const std::vector<IoOutputAliasIdentity> &expected_current_aliases,
      const std::vector<MockBooleanOutputAlias> &candidate_aliases,
      core::ActiveBundlePublicationGeneration active_publication_generation);
  void Shutdown();

  /// Host-owned synthetic echo hook. Called once for each newly accepted
  /// write, in channel order. It must not reenter this provider.
  void SetModelObserver(std::function<void(bool, core::TimePoint)> observer);

  /// Serialize an echo publication with output acceptance. The host first
  /// acquires its provider-delivery barriers and must not call back into this
  /// output provider while holding the returned lock.
  [[nodiscard]] std::unique_lock<std::mutex> AcquireModelDelivery() const;

  /// Actual local synthetic model, initially unwritten. This is not a signal
  /// observation or physical readback and is never published to input ingress.
  [[nodiscard]] std::optional<bool> model_value() const;

private:
  struct State;
  explicit MockBooleanOutputProvider(std::unique_ptr<State> state);
  std::unique_ptr<State> state_;
};

} // namespace manyforge::roboplan_backend
