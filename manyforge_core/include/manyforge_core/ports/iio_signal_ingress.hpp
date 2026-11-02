#pragma once

#include <manyforge_core/types/io_signal.hpp>

namespace manyforge::core {

/// Bounded host ingress, shared directly by providers. Calls may briefly lock;
/// they do not drain/evaluate, call providers, or perform physical effects.
class IIoSignalIngress {
 public:
  virtual ~IIoSignalIngress() = default;
  /// A valid current-source frame refused for capacity records its own loss
  /// barrier exactly once. Invalid/stale frames do not alter current-source
  /// state. A provider must not issue a second NotifyLoss for this refusal.
  virtual bool TryEnqueue(const IoSourceAcquisitionDTO& acquisition) noexcept = 0;
  virtual void NotifyLoss(const IoIngressLossNotificationDTO& loss) noexcept = 0;
};

}  // namespace manyforge::core
