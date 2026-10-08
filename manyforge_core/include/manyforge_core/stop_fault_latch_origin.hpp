#pragma once

#include <cstdint>
#include <manyforge_core/stop_terminal_reducer.hpp>
#include <string_view>

namespace manyforge::core {

/// Why the Stop behind a cell Stop-fault latch was requested. This is
/// traceability shown to the operator only: no latch is ever recovered
/// automatically, whatever its origin (maintainer decision 2026-10-08,
/// addendum 2). Every latch is cleared only by the operator's exact recovery.
///
/// `kProviderAvailabilityLoss` and `kComponentIncident` are attributed only to
/// containment Stops the Kernel issued itself; that attribution is resident, so
/// a replacement Kernel reports the certificate-derived origin or
/// `kUnattributed`.
enum class StopFaultLatchOriginDTO : std::uint8_t {
  kUnattributed = 0,
  kOperatorStop,
  kOperatorFullStop,
  kMalformedStopPromotion,
  kProgramStop,
  /// Containment for a Manager liveness (provider availability) loss.
  kProviderAvailabilityLoss,
  /// Containment CCLC requested for a component incident.
  kComponentIncident,
};

[[nodiscard]] std::string_view to_string(StopFaultLatchOriginDTO value) noexcept;

/// Origin derivable from a terminal certificate alone: an operator, Program or
/// malformed-Stop origin, never one of the Kernel's own containment origins.
[[nodiscard]] StopFaultLatchOriginDTO stop_fault_latch_certificate_origin(
    const StopCertificateDTO& certificate) noexcept;

}  // namespace manyforge::core
