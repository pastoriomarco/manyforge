#pragma once

#include <manyforge_core/types/strong_id.hpp>
#include <optional>

namespace manyforge::core {

struct CapabilityProviderBindingIdentityDTO final {
  CapabilityProviderBindingId capability_provider_binding_id;
  CapabilityProviderBindingRevision capability_provider_binding_revision;

  friend bool operator==(const CapabilityProviderBindingIdentityDTO&,
                         const CapabilityProviderBindingIdentityDTO&) = default;
};

struct ControllerParticipantBindingIdentityDTO final {
  ControlBindingId control_binding_id;
  ControlBindingRevision control_binding_revision;
  std::optional<CapabilityProviderBindingIdentityDTO> capability_provider{};

  friend bool operator==(const ControllerParticipantBindingIdentityDTO&,
                         const ControllerParticipantBindingIdentityDTO&) = default;
};

/// Same exact authority identity in a resolved binding, prepared product and
/// participant observation.
struct ParticipantBindingIdentityDTO final {
  ControlDomainId control_domain_id;
  ControlDomainRevision control_domain_revision;
  ControllerParticipantBindingIdentityDTO controller;

  [[nodiscard]] const CapabilityProviderBindingIdentityDTO* capability_provider() const noexcept {
    return controller.capability_provider ? &*controller.capability_provider : nullptr;
  }
  [[nodiscard]] bool locally_valid() const noexcept {
    return !control_domain_id.empty() && !control_domain_revision.empty() &&
           !controller.control_binding_id.empty() && !controller.control_binding_revision.empty() &&
           (!controller.capability_provider ||
            (!controller.capability_provider->capability_provider_binding_id.empty() &&
             !controller.capability_provider->capability_provider_binding_revision.empty()));
  }

  friend bool operator==(const ParticipantBindingIdentityDTO&,
                         const ParticipantBindingIdentityDTO&) = default;
};

}  // namespace manyforge::core
