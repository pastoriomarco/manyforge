#pragma once

#include <manyforge_core/types/external_observed_state.hpp>

namespace manyforge::core {

/// Transport-neutral receive-only seam for an external robot/perception state
/// adapter. It deliberately exposes no command, scene-mutation, discovery, or
/// caller-authorization operation.
class IExternalObservedStateReceiver {
 public:
  virtual ~IExternalObservedStateReceiver() = default;

  [[nodiscard]] virtual Result<ExternalObservedStateIngestDisposition> ingest(
      ExternalObservedStatePublicationDTO publication) = 0;
};

}  // namespace manyforge::core
