#pragma once

#include <manyforge_core/types/component_lifecycle.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/io_signal_catalog.hpp>
#include <manyforge_deployment/provider_manifest_codec.hpp>
#include <manyforge_deployment/runtime_profile_codec.hpp>
#include <span>

namespace manyforge::deployment {

struct IoSignalControlDomainBinding final {
  core::IoSignalId signal_id;
  core::ControlDomainId control_domain_id;
  friend bool operator==(const IoSignalControlDomainBinding &,
                         const IoSignalControlDomainBinding &) = default;
};

/// Compiler-derived candidate data, not lifecycle readiness or admission.
/// Existing arm domains are referenced, never copied or revised for GPIO.
struct IoProviderDomainDerivation final {
  std::vector<core::ControlDomainDTO> provider_domains;
  std::vector<IoSignalControlDomainBinding> signal_domains;
};

/// Requires normalized runtime configuration and exact qualified manifests,
/// runtime profiles.
[[nodiscard]] core::Result<IoProviderDomainDerivation> DeriveIoProviderDomains(
    const core::CellGraphDTO &graph,
    std::span<const core::CapabilityProviderBindingDTO> provider_bindings,
    std::span<const QualifiedRuntimeProfile> runtime_profiles,
    std::span<const QualifiedProviderManifest> provider_manifests);

/// Exact provider alternative for the existing graph codec/identity pipeline.
/// Controller records are intentionally outside this projection.
[[nodiscard]] CanonicalValue
IoProviderDomainRecord(const core::ControlDomainDTO &domain);

/// Recomputes ownership/member proofs and requires exact provider coverage.
/// Existing graph revision verification remains a separate prerequisite.
[[nodiscard]] core::Result<void> ValidateIoProviderDomains(
    const core::CellGraphDTO &graph,
    std::span<const core::CapabilityProviderBindingDTO> provider_bindings,
    std::span<const QualifiedRuntimeProfile> runtime_profiles,
    std::span<const QualifiedProviderManifest> provider_manifests);

/// Closed structural readback only: recomputes ownership and member rows.
[[nodiscard]] core::Result<void> ValidateIoProviderDomainProjection(
    const core::CellGraphDTO &graph,
    std::span<const core::CapabilityProviderBindingDTO> provider_bindings,
    std::span<const QualifiedRuntimeProfile> runtime_profiles);

} // namespace manyforge::deployment
