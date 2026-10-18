#pragma once

#include <manyforge_core/types/strong_id.hpp>
#include <optional>
#include <variant>

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
/// participant observation. A provider command has a genuine domain and
/// capability binding; it does not manufacture a motion control binding.
struct ParticipantBindingIdentityDTO final {
  ControlDomainId control_domain_id;
  ControlDomainRevision control_domain_revision;
  std::variant<ControllerParticipantBindingIdentityDTO,
               CapabilityProviderBindingIdentityDTO> binding;

  [[nodiscard]] const ControllerParticipantBindingIdentityDTO* controller() const noexcept {
    return std::get_if<ControllerParticipantBindingIdentityDTO>(&binding);
  }
  [[nodiscard]] const CapabilityProviderBindingIdentityDTO* provider() const noexcept {
    return std::get_if<CapabilityProviderBindingIdentityDTO>(&binding);
  }
  [[nodiscard]] const CapabilityProviderBindingIdentityDTO* capability_provider() const noexcept {
    if (const auto* physical = controller())
      return physical->capability_provider ? &*physical->capability_provider : nullptr;
    return provider();
  }
  [[nodiscard]] bool locally_valid() const noexcept {
    if (binding.valueless_by_exception() || control_domain_id.empty() || control_domain_revision.empty()) return false;
    const auto valid_provider = [](const auto& value) {
      return !value.capability_provider_binding_id.empty() &&
             !value.capability_provider_binding_revision.empty();
    };
    if (const auto* physical = controller()) {
      return !physical->control_binding_id.empty() && !physical->control_binding_revision.empty() &&
             (!physical->capability_provider || valid_provider(*physical->capability_provider));
    }
    return valid_provider(*provider());
  }

  friend bool operator==(const ParticipantBindingIdentityDTO&,
                         const ParticipantBindingIdentityDTO&) = default;
};

}  // namespace manyforge::core
