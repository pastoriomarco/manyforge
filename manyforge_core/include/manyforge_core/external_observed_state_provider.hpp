#pragma once

#include <manyforge_core/ports/iexternal_observed_state_receiver.hpp>
#include <manyforge_core/ports/iobserved_state_provider.hpp>
#include <memory>
#include <mutex>
#include <shared_mutex>

namespace manyforge::core {

/// Receiver-local immutable publication cache for one exact external source.
///
/// A ROS/DDS, remote-agent, shared-memory, or test adapter decodes its wire
/// message and invokes `ingest`. Consumers continue to use the unchanged
/// `IObservedStateProvider`; transport placement never leaks into kinematic
/// state, planning, collision, command, or scene contracts.
class ExternalObservedStateProvider final : public IExternalObservedStateReceiver,
                                            public IObservedStateProvider {
 public:
  explicit ExternalObservedStateProvider(ExternalObservedStateSourceContractDTO contract);
  ~ExternalObservedStateProvider() override;

  ExternalObservedStateProvider(const ExternalObservedStateProvider&) = delete;
  ExternalObservedStateProvider& operator=(const ExternalObservedStateProvider&) = delete;

  [[nodiscard]] Result<ExternalObservedStateIngestDisposition> ingest(
      ExternalObservedStatePublicationDTO publication) override;

  [[nodiscard]] Result<ObservedStateSnapshotDTO> current_state(
      const ObservedStateQueryContextDTO& context, std::int64_t now_ns) const override;

  [[nodiscard]] Result<ObservedStatePublicationDTO> publication(
      const ControlBindingId& control_binding_id,
      const ControlBindingRevision& control_binding_revision) const override;

  [[nodiscard]] const ExternalObservedStateSourceContractDTO& contract() const noexcept {
    return contract_;
  }

  [[nodiscard]] const ContentDigest& contract_digest() const noexcept { return contract_digest_; }

 private:
  struct LatestPublication;

  ExternalObservedStateSourceContractDTO contract_;
  ContentDigest contract_digest_;
  mutable std::shared_mutex publication_mutex_;
  std::shared_ptr<const LatestPublication> latest_;
};

}  // namespace manyforge::core
