#include <manyforge_core/stop_fault_latch_origin.hpp>

namespace manyforge::core {

std::string_view to_string(const StopFaultLatchOriginDTO value) noexcept {
  switch (value) {
    case StopFaultLatchOriginDTO::kUnattributed:
      return "unattributed";
    case StopFaultLatchOriginDTO::kOperatorStop:
      return "operator_stop";
    case StopFaultLatchOriginDTO::kOperatorFullStop:
      return "operator_full_stop";
    case StopFaultLatchOriginDTO::kMalformedStopPromotion:
      return "malformed_stop_promotion";
    case StopFaultLatchOriginDTO::kProgramStop:
      return "program_stop";
    case StopFaultLatchOriginDTO::kProviderAvailabilityLoss:
      return "provider_availability_loss";
    case StopFaultLatchOriginDTO::kComponentIncident:
      return "component_incident";
  }
  return "unattributed";
}

StopFaultLatchOriginDTO stop_fault_latch_certificate_origin(
    const StopCertificateDTO& certificate) noexcept {
  switch (certificate.requested_selector.kind) {
    case StopRequestedSelectorKindDTO::kOrdinaryOperations:
    case StopRequestedSelectorKindDTO::kOrdinaryComponents:
    case StopRequestedSelectorKindDTO::kOrdinaryResources:
      return certificate.resolution_disposition == StopResolutionDispositionDTO::kPromotedToFull
                 ? StopFaultLatchOriginDTO::kMalformedStopPromotion
                 : StopFaultLatchOriginDTO::kOperatorStop;
    case StopRequestedSelectorKindDTO::kExplicitFull:
      return StopFaultLatchOriginDTO::kOperatorFullStop;
    case StopRequestedSelectorKindDTO::kInternalProgramRun:
      return StopFaultLatchOriginDTO::kProgramStop;
    case StopRequestedSelectorKindDTO::kMalformedIngress:
      return StopFaultLatchOriginDTO::kMalformedStopPromotion;
  }
  return StopFaultLatchOriginDTO::kUnattributed;
}

}  // namespace manyforge::core
