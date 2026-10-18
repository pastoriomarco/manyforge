#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace manyforge::core {

/// Adapter-classified reason why a component is not at its requested
/// lifecycle target (ADR-0026 §7 lifecycle-target amendment, 2026-10-08).
///
/// Adapters report the class as the token value of the standard lifecycle fact
/// `kLifecycleBlockingCauseFactId`. A class the adapter cannot establish from
/// fresh controller status is `kUnknown`, which is never recovered
/// automatically. `kAvailability` means status or feedback is unreachable or
/// stale before the component first reaches its target; fresh status that is
/// simply not ready yet is `kNone`. Every other class requires intervention:
/// CCLC retains it until the operator's Recover. A fresh intervention from
/// the current occurrence is an incident in every lifecycle state, except
/// during an unadopted whole-component Apply replacement (maintainer129).
enum class LifecycleBlockingCauseClassDTO : std::uint8_t {
  kNone = 0,
  kAvailability,
  kEmergencyStop,
  kProtectiveStop,
  kSafeguardStop,
  kFault,
  kConfiguration,
  kUnknown,
  /// Vendor-reported collision stop; kept separate from `kFault` so its
  /// recovery policy can differ later.
  kCollision,
  /// The robot was moved, or its mode/state changed, from its own interface
  /// (for example a vendor-UI jog) without a ManyForge request.
  kExternalControl,
  /// The provider or controller was lost after reaching its target: crash,
  /// restart, unrequested deactivation or expired communication lease.
  kProviderLost,
};

inline constexpr std::string_view kLifecycleBlockingCauseFactId =
    "manyforge.blocking_cause_class";

/// Prefix of the adapter's diagnostic vendor-code facts, reported only from
/// fresh vendor status (`manyforge.vendor_code.error` token `C2`,
/// `.power_cycle` token `power_cycle_required`, `.warning`/`.mode`/`.state`
/// integers). CCLC copies them into incident `vendor_codes`; they are never
/// lifecycle plan predicate inputs.
inline constexpr std::string_view kLifecycleVendorCodeFactPrefix = "manyforge.vendor_code.";

[[nodiscard]] constexpr std::string_view to_string(
    const LifecycleBlockingCauseClassDTO value) noexcept {
  switch (value) {
    case LifecycleBlockingCauseClassDTO::kNone:
      return "none";
    case LifecycleBlockingCauseClassDTO::kAvailability:
      return "availability";
    case LifecycleBlockingCauseClassDTO::kEmergencyStop:
      return "emergency_stop";
    case LifecycleBlockingCauseClassDTO::kProtectiveStop:
      return "protective_stop";
    case LifecycleBlockingCauseClassDTO::kSafeguardStop:
      return "safeguard_stop";
    case LifecycleBlockingCauseClassDTO::kFault:
      return "fault";
    case LifecycleBlockingCauseClassDTO::kConfiguration:
      return "configuration";
    case LifecycleBlockingCauseClassDTO::kUnknown:
      return "unknown";
    case LifecycleBlockingCauseClassDTO::kCollision:
      return "collision";
    case LifecycleBlockingCauseClassDTO::kExternalControl:
      return "external_control";
    case LifecycleBlockingCauseClassDTO::kProviderLost:
      return "provider_lost";
  }
  return "unknown";
}

[[nodiscard]] constexpr std::optional<LifecycleBlockingCauseClassDTO>
parse_lifecycle_blocking_cause_class(const std::string_view value) noexcept {
  for (const auto candidate :
       {LifecycleBlockingCauseClassDTO::kNone,
        LifecycleBlockingCauseClassDTO::kAvailability,
        LifecycleBlockingCauseClassDTO::kEmergencyStop,
        LifecycleBlockingCauseClassDTO::kProtectiveStop,
        LifecycleBlockingCauseClassDTO::kSafeguardStop,
        LifecycleBlockingCauseClassDTO::kFault,
        LifecycleBlockingCauseClassDTO::kConfiguration,
        LifecycleBlockingCauseClassDTO::kUnknown,
        LifecycleBlockingCauseClassDTO::kCollision,
        LifecycleBlockingCauseClassDTO::kExternalControl,
        LifecycleBlockingCauseClassDTO::kProviderLost}) {
    if (to_string(candidate) == value) return candidate;
  }
  return std::nullopt;
}

/// True when the class requires an intervention or an explicitly authorized
/// recovery step before progress toward the target may continue. Such a
/// class survives loss of communication until fresh controller status clears it.
[[nodiscard]] constexpr bool lifecycle_cause_requires_intervention(
    const LifecycleBlockingCauseClassDTO value) noexcept {
  return value != LifecycleBlockingCauseClassDTO::kNone &&
         value != LifecycleBlockingCauseClassDTO::kAvailability;
}

}  // namespace manyforge::core
