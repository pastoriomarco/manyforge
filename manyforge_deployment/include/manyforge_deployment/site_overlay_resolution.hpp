#pragma once

#include <manyforge_core/types/deployment_applicability.hpp>

#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace manyforge::deployment {

class SiteOverlayResolutionError : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

/// Value-free projection independently attested by the outer site resolver
/// and structurally verified at the deployment-compiler boundary.
///
/// The private constructor prevents ordinary deployment/compiler code from
/// blessing an arbitrary reference table. The sole producer parses the exact
/// standalone artifact, verifies its semantic revision and normalized digest,
/// and then projects only external-resource reference/kind keys.
class VerifiedSiteOverlayArtifactProjection final {
 public:
  VerifiedSiteOverlayArtifactProjection(
      const VerifiedSiteOverlayArtifactProjection&) = default;
  VerifiedSiteOverlayArtifactProjection& operator=(
      const VerifiedSiteOverlayArtifactProjection&) = default;

  [[nodiscard]] const core::SiteOverlayRefDTO& reference() const noexcept {
    return reference_;
  }
  [[nodiscard]] std::span<const core::SiteOverlayExternalResourceKeyDTO>
  external_resource_keys() const noexcept {
    return external_resource_keys_;
  }

 private:
  explicit VerifiedSiteOverlayArtifactProjection(
      core::SiteOverlayRefDTO reference,
      std::vector<core::SiteOverlayExternalResourceKeyDTO>
          external_resource_keys)
      : reference_(std::move(reference)),
        external_resource_keys_(std::move(external_resource_keys)) {}

  core::SiteOverlayRefDTO reference_;
  std::vector<core::SiteOverlayExternalResourceKeyDTO>
      external_resource_keys_;

  friend VerifiedSiteOverlayArtifactProjection
  resolve_and_verify_site_overlay_artifact_projection(
      const core::SiteOverlayRefDTO&, std::string_view);
};

/// Exact optional overlay selection qualified against one deployment.
///
/// An empty result is valid only when the deployment has neither a semantic
/// overlay reference nor external-resource rows. A non-empty result owns the
/// exact selected reference and canonical value-free key index; it contains
/// no resolved values, secrets, credentials, concrete endpoints, or runtime
/// readiness.
class QualifiedSelectedSiteOverlay final {
 public:
  QualifiedSelectedSiteOverlay(const QualifiedSelectedSiteOverlay&) = default;
  QualifiedSelectedSiteOverlay& operator=(const QualifiedSelectedSiteOverlay&) =
      default;

  [[nodiscard]] bool has_selection() const noexcept {
    return selection_.has_value();
  }
  [[nodiscard]] const core::SiteOverlayRefDTO* reference() const noexcept {
    return selection_.has_value() ? &selection_->reference() : nullptr;
  }
  [[nodiscard]] std::span<const core::SiteOverlayExternalResourceKeyDTO>
  external_resource_keys() const noexcept {
    return selection_.has_value() ? selection_->external_resource_keys()
                                  : std::span<
                                        const core::
                                            SiteOverlayExternalResourceKeyDTO>{};
  }

 private:
  explicit QualifiedSelectedSiteOverlay(
      std::optional<VerifiedSiteOverlayArtifactProjection> selection)
      : selection_(std::move(selection)) {}

  std::optional<VerifiedSiteOverlayArtifactProjection> selection_;

  friend QualifiedSelectedSiteOverlay select_site_overlay_for_deployment(
      const std::optional<core::SiteOverlayRefDTO>&,
      std::span<const core::ExternalResourceDTO>,
      const VerifiedSiteOverlayArtifactProjection*);
};

/// Parses and verifies one resolver-retrieved `manyforge.site_overlay.v1`
/// artifact against the exact deployment reference.
///
/// The full artifact is closed, normalized, and checked under the
/// `site-overlay-revision` semantic identity. `non_secret_digest` must equal
/// SHA-256 of the complete RFC 8785 normalized artifact after revision
/// verification. Resolved values are validated but discarded: the returned
/// trust root exposes only external-resource reference/kind keys.
[[nodiscard]] VerifiedSiteOverlayArtifactProjection
resolve_and_verify_site_overlay_artifact_projection(
    const core::SiteOverlayRefDTO& expected_reference,
    std::string_view stored_bytes);

/// Applies the exact site-overlay selection truth table for one deployment.
///
/// `selected_projection == nullptr` means the outer resolver supplied no
/// selected artifact. Absence and explicit null have already collapsed to
/// `deployment_reference == std::nullopt`; their source spelling remains in
/// the deployment codec wrapper. Every external-resource row must resolve an
/// exact `(site_overlay_value_ref, resource_kind)` key.
[[nodiscard]] QualifiedSelectedSiteOverlay select_site_overlay_for_deployment(
    const std::optional<core::SiteOverlayRefDTO>& deployment_reference,
    std::span<const core::ExternalResourceDTO> external_resources,
    const VerifiedSiteOverlayArtifactProjection* selected_projection);

}  // namespace manyforge::deployment
