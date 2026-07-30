#pragma once

#include <cstdint>
#include <manyforge_core/types/deployment_applicability.hpp>
#include <optional>
#include <stdexcept>
#include <utility>

#include "manyforge_deployment/artifact_codec.hpp"
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
      const TypedDeploymentApplicabilityInputsCandidate&) = default;
  TypedDeploymentApplicabilityInputsCandidate& operator=(
      const TypedDeploymentApplicabilityInputsCandidate&) = default;

  [[nodiscard]] const core::DeploymentApplicabilityInputsDTO& value()
      const noexcept {
    return value_;
  }
  [[nodiscard]] const CanonicalValue&
  normalized_desired_control_configurations() const noexcept {
    return normalized_desired_control_configurations_;
  }
  [[nodiscard]] const CanonicalValue&
  normalized_capability_provider_bindings() const noexcept {
    return normalized_capability_provider_bindings_;
  }
  [[nodiscard]] const CanonicalValue& normalized_external_resources()
      const noexcept {
    return normalized_external_resources_;
  }
  [[nodiscard]] SiteOverlayRefSourcePresence site_overlay_ref_presence()
      const noexcept {
    return site_overlay_ref_presence_;
  }
  /// Nullopt means the source member was absent. A present CanonicalValue is
  /// either JSON null or the normalized exact reference object.
  [[nodiscard]] const std::optional<CanonicalValue>&
  normalized_site_overlay_ref() const noexcept {
    return normalized_site_overlay_ref_;
  }
  [[nodiscard]] bool external_resources_was_present() const noexcept {
    return external_resources_was_present_;
  }

 private:
  TypedDeploymentApplicabilityInputsCandidate(
      core::DeploymentApplicabilityInputsDTO value,
      CanonicalValue normalized_desired_control_configurations,
      CanonicalValue normalized_capability_provider_bindings,
      CanonicalValue normalized_external_resources,
      SiteOverlayRefSourcePresence site_overlay_ref_presence,
      std::optional<CanonicalValue> normalized_site_overlay_ref,
      bool external_resources_was_present)
      : value_(std::move(value)),
        normalized_desired_control_configurations_(
            std::move(normalized_desired_control_configurations)),
        normalized_capability_provider_bindings_(
            std::move(normalized_capability_provider_bindings)),
        normalized_external_resources_(
            std::move(normalized_external_resources)),
        site_overlay_ref_presence_(site_overlay_ref_presence),
        normalized_site_overlay_ref_(std::move(normalized_site_overlay_ref)),
        external_resources_was_present_(external_resources_was_present) {}

  core::DeploymentApplicabilityInputsDTO value_;
  CanonicalValue normalized_desired_control_configurations_;
  CanonicalValue normalized_capability_provider_bindings_;
  CanonicalValue normalized_external_resources_;
  SiteOverlayRefSourcePresence site_overlay_ref_presence_{
      SiteOverlayRefSourcePresence::kAbsent};
  std::optional<CanonicalValue> normalized_site_overlay_ref_;
  bool external_resources_was_present_{false};

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
      const RevisionVerifiedDeploymentApplicabilityInputs&) = default;
  RevisionVerifiedDeploymentApplicabilityInputs& operator=(
      const RevisionVerifiedDeploymentApplicabilityInputs&) = default;

  [[nodiscard]] const core::DeploymentApplicabilityInputsDTO& value()
      const noexcept {
    return value_;
  }
  [[nodiscard]] const CanonicalValue&
  normalized_desired_control_configurations() const noexcept {
    return normalized_desired_control_configurations_;
  }
  [[nodiscard]] const CanonicalValue&
  normalized_capability_provider_bindings() const noexcept {
    return normalized_capability_provider_bindings_;
  }
  [[nodiscard]] const CanonicalValue& normalized_external_resources()
      const noexcept {
    return normalized_external_resources_;
  }
  [[nodiscard]] SiteOverlayRefSourcePresence site_overlay_ref_presence()
      const noexcept {
    return site_overlay_ref_presence_;
  }
  /// Nullopt means the source member was absent. A present CanonicalValue is
  /// either JSON null or the normalized exact reference object.
  [[nodiscard]] const std::optional<CanonicalValue>&
  normalized_site_overlay_ref() const noexcept {
    return normalized_site_overlay_ref_;
  }
  [[nodiscard]] bool external_resources_was_present() const noexcept {
    return external_resources_was_present_;
  }

 private:
  RevisionVerifiedDeploymentApplicabilityInputs(
      core::DeploymentApplicabilityInputsDTO value,
      CanonicalValue normalized_desired_control_configurations,
      CanonicalValue normalized_capability_provider_bindings,
      CanonicalValue normalized_external_resources,
      SiteOverlayRefSourcePresence site_overlay_ref_presence,
      std::optional<CanonicalValue> normalized_site_overlay_ref,
      bool external_resources_was_present)
      : value_(std::move(value)),
        normalized_desired_control_configurations_(
            std::move(normalized_desired_control_configurations)),
        normalized_capability_provider_bindings_(
            std::move(normalized_capability_provider_bindings)),
        normalized_external_resources_(
            std::move(normalized_external_resources)),
        site_overlay_ref_presence_(site_overlay_ref_presence),
        normalized_site_overlay_ref_(std::move(normalized_site_overlay_ref)),
        external_resources_was_present_(external_resources_was_present) {}

  core::DeploymentApplicabilityInputsDTO value_;
  CanonicalValue normalized_desired_control_configurations_;
  CanonicalValue normalized_capability_provider_bindings_;
  CanonicalValue normalized_external_resources_;
  SiteOverlayRefSourcePresence site_overlay_ref_presence_{
      SiteOverlayRefSourcePresence::kAbsent};
  std::optional<CanonicalValue> normalized_site_overlay_ref_;
  bool external_resources_was_present_{false};

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
