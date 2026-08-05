#pragma once

#include <cstdint>
#include <manyforge_core/types/deployment_applicability.hpp>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

#include "manyforge_deployment/artifact_codec.hpp"
#include "manyforge_deployment/assistant_settings_codec.hpp"
#include "manyforge_deployment/canonical_value.hpp"

namespace manyforge::deployment {

class DeploymentApplicabilityCodecError : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

/// Source spelling of the optional deployment `site_overlay_ref` member.
///
/// Absent and explicit null have the same semantic meaning (no selected
/// overlay) but remain distinct normalized source states for final deployment
/// identity reconstruction.
enum class SiteOverlayRefSourcePresence : std::uint8_t {
  kAbsent = 0,
  kExplicitNull,
  kValue,
};

class RevisionVerifiedDeploymentApplicabilityInputs;

/// Strictly typed and collection-normalized deployment applicability inputs
/// whose declared capability-provider-binding revisions remain untrusted.
///
/// The decoder consumes the three named members from a strictly decoded
/// `manyforge.deployment.v1` root. It deliberately does not claim that the
/// rest of the deployment root is complete or closed; the final deployment
/// compiler owns that envelope. Every nested record handled here is closed.
class TypedDeploymentApplicabilityInputsCandidate final {
 public:
  TypedDeploymentApplicabilityInputsCandidate(
      const TypedDeploymentApplicabilityInputsCandidate&) noexcept = default;
  TypedDeploymentApplicabilityInputsCandidate& operator=(
      const TypedDeploymentApplicabilityInputsCandidate&) noexcept = default;

  [[nodiscard]] const core::DeploymentApplicabilityInputsDTO& value()
      const noexcept {
    return data_->value;
  }
  [[nodiscard]] const CanonicalValue&
  normalized_desired_control_configurations() const noexcept {
    return data_->normalized_desired_control_configurations;
  }
  [[nodiscard]] const CanonicalValue&
  normalized_capability_provider_bindings() const noexcept {
    return data_->normalized_capability_provider_bindings;
  }
  [[nodiscard]] const CanonicalValue& normalized_external_resources()
      const noexcept {
    return data_->normalized_external_resources;
  }
  [[nodiscard]] SiteOverlayRefSourcePresence site_overlay_ref_presence()
      const noexcept {
    return data_->site_overlay_ref_presence;
  }
  /// Nullopt means the source member was absent. A present CanonicalValue is
  /// either JSON null or the normalized exact reference object.
  [[nodiscard]] const std::optional<CanonicalValue>&
  normalized_site_overlay_ref() const noexcept {
    return data_->normalized_site_overlay_ref;
  }
  [[nodiscard]] bool external_resources_was_present() const noexcept {
    return data_->external_resources_was_present;
  }
  [[nodiscard]] const TypedAssistantSettingsCandidate& assistant_settings()
      const noexcept {
    return data_->assistant_settings;
  }

 private:
  struct Data final {
    Data(core::DeploymentApplicabilityInputsDTO value_in,
         CanonicalValue normalized_desired_control_configurations_in,
         CanonicalValue normalized_capability_provider_bindings_in,
         CanonicalValue normalized_external_resources_in,
         SiteOverlayRefSourcePresence site_overlay_ref_presence_in,
         std::optional<CanonicalValue> normalized_site_overlay_ref_in,
         bool external_resources_was_present_in,
         const TypedAssistantSettingsCandidate& assistant_settings_in)
        : value(std::move(value_in)),
          normalized_desired_control_configurations(
              std::move(normalized_desired_control_configurations_in)),
          normalized_capability_provider_bindings(
              std::move(normalized_capability_provider_bindings_in)),
          normalized_external_resources(
              std::move(normalized_external_resources_in)),
          site_overlay_ref_presence(site_overlay_ref_presence_in),
          normalized_site_overlay_ref(
              std::move(normalized_site_overlay_ref_in)),
          external_resources_was_present(external_resources_was_present_in),
          assistant_settings(assistant_settings_in) {}

    core::DeploymentApplicabilityInputsDTO value;
    CanonicalValue normalized_desired_control_configurations;
    CanonicalValue normalized_capability_provider_bindings;
    CanonicalValue normalized_external_resources;
    SiteOverlayRefSourcePresence site_overlay_ref_presence{
        SiteOverlayRefSourcePresence::kAbsent};
    std::optional<CanonicalValue> normalized_site_overlay_ref;
    bool external_resources_was_present{false};
    TypedAssistantSettingsCandidate assistant_settings;
  };

  TypedDeploymentApplicabilityInputsCandidate(
      core::DeploymentApplicabilityInputsDTO value,
      CanonicalValue normalized_desired_control_configurations,
      CanonicalValue normalized_capability_provider_bindings,
      CanonicalValue normalized_external_resources,
      SiteOverlayRefSourcePresence site_overlay_ref_presence,
      std::optional<CanonicalValue> normalized_site_overlay_ref,
      bool external_resources_was_present,
      const TypedAssistantSettingsCandidate& assistant_settings)
      : data_(std::make_shared<const Data>(
            std::move(value),
            std::move(normalized_desired_control_configurations),
            std::move(normalized_capability_provider_bindings),
            std::move(normalized_external_resources),
            site_overlay_ref_presence,
            std::move(normalized_site_overlay_ref),
            external_resources_was_present, assistant_settings)) {}

  std::shared_ptr<const Data> data_;

  friend TypedDeploymentApplicabilityInputsCandidate
  decode_deployment_applicability_inputs_candidate(
      const ParsedArtifactCandidate&);
  friend class RevisionVerifiedDeploymentApplicabilityInputs;
  friend RevisionVerifiedDeploymentApplicabilityInputs
  compute_capability_provider_binding_revisions(
      const TypedDeploymentApplicabilityInputsCandidate&);
  friend RevisionVerifiedDeploymentApplicabilityInputs
  verify_capability_provider_binding_revisions(
      const TypedDeploymentApplicabilityInputsCandidate&);
};

/// Exact normalized input tables whose nested
/// CapabilityProviderBindingRevision values have been independently minted or
/// verified.
///
/// This boundary proves only closed shape, strict scalar syntax, canonical
/// collection order, and binding semantic identity. Graph occurrence,
/// catalog, provider, runtime-profile, topology, and site-overlay
/// applicability remain the later C0.2n qualifier's responsibility.
class RevisionVerifiedDeploymentApplicabilityInputs final {
 public:
  RevisionVerifiedDeploymentApplicabilityInputs(
      const RevisionVerifiedDeploymentApplicabilityInputs&) noexcept =
      default;
  RevisionVerifiedDeploymentApplicabilityInputs& operator=(
      const RevisionVerifiedDeploymentApplicabilityInputs&) noexcept =
      default;

  [[nodiscard]] const core::DeploymentApplicabilityInputsDTO& value()
      const noexcept {
    return data_->candidate.value();
  }
  [[nodiscard]] const CanonicalValue&
  normalized_desired_control_configurations() const noexcept {
    return data_->candidate.normalized_desired_control_configurations();
  }
  [[nodiscard]] const CanonicalValue&
  normalized_capability_provider_bindings() const noexcept {
    return data_->candidate.normalized_capability_provider_bindings();
  }
  [[nodiscard]] const CanonicalValue& normalized_external_resources()
      const noexcept {
    return data_->candidate.normalized_external_resources();
  }
  [[nodiscard]] SiteOverlayRefSourcePresence site_overlay_ref_presence()
      const noexcept {
    return data_->candidate.site_overlay_ref_presence();
  }
  /// Nullopt means the source member was absent. A present CanonicalValue is
  /// either JSON null or the normalized exact reference object.
  [[nodiscard]] const std::optional<CanonicalValue>&
  normalized_site_overlay_ref() const noexcept {
    return data_->candidate.normalized_site_overlay_ref();
  }
  [[nodiscard]] bool external_resources_was_present() const noexcept {
    return data_->candidate.external_resources_was_present();
  }
  [[nodiscard]] const TypedAssistantSettingsCandidate& assistant_settings()
      const noexcept {
    return data_->candidate.assistant_settings();
  }

 private:
  struct Data final {
    explicit Data(
        const TypedDeploymentApplicabilityInputsCandidate& candidate_in)
        : candidate(candidate_in) {}

    TypedDeploymentApplicabilityInputsCandidate candidate;
  };

  explicit RevisionVerifiedDeploymentApplicabilityInputs(
      const TypedDeploymentApplicabilityInputsCandidate& candidate)
      : data_(std::make_shared<const Data>(candidate)) {}

  std::shared_ptr<const Data> data_;

  friend RevisionVerifiedDeploymentApplicabilityInputs
  compute_capability_provider_binding_revisions(
      const TypedDeploymentApplicabilityInputsCandidate&);
  friend RevisionVerifiedDeploymentApplicabilityInputs
  verify_capability_provider_binding_revisions(
      const TypedDeploymentApplicabilityInputsCandidate&);
};

/// Decodes only the C0.2n-owned tables from one strictly parsed deployment
/// root.
///
/// `api_version`, `desired_control_configurations`, and
/// `capability_provider_bindings` are required. `external_resources` is
/// optional-by-absence and projects to an empty semantic table when absent;
/// `external_resources_was_present()` preserves that distinction for the
/// complete deployment identity stage. `site_overlay_ref` is
/// optional-by-absence and may be explicit null; both mean no semantic
/// selection while the three source states are retained exactly. Other
/// top-level fields are ignored by this focused substrate and must be closed
/// by the final deployment codec.
///
/// Desired rows normalize by `(control_domain_id,
/// control_domain_revision)`, provider rows by
/// `(capability_provider_binding_id,
/// capability_provider_binding_revision)`, desired routes by their complete
/// three-field identity, endpoint rows by `endpoint_role_id`, and external
/// rows by `external_resource_alias`. Duplicate complete keys reject.
[[nodiscard]] TypedDeploymentApplicabilityInputsCandidate
decode_deployment_applicability_inputs_candidate(
    const ParsedArtifactCandidate& parsed_deployment);

/// Producer path: replace every declared provider-binding revision with its
/// exact spec-210 semantic identity, then renormalize the outer table.
[[nodiscard]] RevisionVerifiedDeploymentApplicabilityInputs
compute_capability_provider_binding_revisions(
    const TypedDeploymentApplicabilityInputsCandidate& candidate);

/// Consumer path: independently verify every declared provider-binding
/// revision without repairing candidate data.
[[nodiscard]] RevisionVerifiedDeploymentApplicabilityInputs
verify_capability_provider_binding_revisions(
    const TypedDeploymentApplicabilityInputsCandidate& candidate);

}  // namespace manyforge::deployment
