#pragma once

#include <cstddef>
#include <manyforge_core/types/retained_product.hpp>
#include <manyforge_core/types/stop_control.hpp>
#include <manyforge_core/types/stop_evidence_policy.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <span>
#include <optional>
#include <string>
#include <string_view>

namespace manyforge::deployment {

class CompilerQualifiedCellIr;
class QualifiedControllerTopology;
class QualifiedProviderManifest;
class QualifiedStopEvidencePolicy;

namespace detail {

inline constexpr std::string_view kControlDomainControlApiVersion =
    "manyforge.control_domain_control.v2";
inline constexpr std::string_view kControlDomainControlContractId =
    "manyforge.control_domain_control";
inline constexpr std::string_view kControlDomainControlArtifactRole = "control_domain_control";
inline constexpr std::string_view kControlDomainControlMediaType =
    "application/vnd.manyforge.control-domain-control.v2+json";
inline constexpr std::size_t kMaxControlDomainControlBytes = 16U * 1024U * 1024U;

/// Stable bundle-local product identity for one exact control domain.
[[nodiscard]] std::string control_domain_control_product_id(
    const core::ControlDomainId& control_domain_id);

/// Emit the closed build-qualified reference control product for one domain.
///
/// The current profile maps I2-qualified direct and two-controller chained
/// topologies to exact offline controller implementation/configuration rows.
/// It performs no runtime lookup, installation, activation, or ROS effect.
[[nodiscard]] std::string emit_control_domain_control(
    const CompilerQualifiedCellIr& compiler_ir,
    const core::ControlDomainId& control_domain_id,
    std::span<const core::StopImplementationQualificationDTO>
        stop_implementation_qualifications = {},
    std::span<const core::ControlledStopSubstitutionQualificationDTO>
        controlled_stop_substitution_qualifications = {});

/// Parse and independently verify one canonical closed artifact.
[[nodiscard]] CanonicalValue parse_and_verify_control_domain_control(
    std::string_view canonical_bytes);

/// Complete byte closure of one retained v2 control-domain product.
/// Conditional members are present exactly when the control artifact retains
/// the corresponding trusted rows.
struct NativeRuntimeBuildIdentityArtifact final {
  core::ProductId product_id;
  std::string_view canonical_bytes;
};

struct ControlDomainControlArtifactClosure final {
  std::string_view control_domain_control;
  std::optional<std::string_view> stop_implementation_qualification_evidence;
  std::optional<std::string_view>
      stop_controlled_substitution_qualification_evidence;
  std::optional<std::string_view> stop_interface_commissioning_evidence;
  std::span<const NativeRuntimeBuildIdentityArtifact>
      native_runtime_build_identities;
};

/// Require byte-for-byte semantic equality with the projection independently
/// derived from the final non-caller-constructible I2 value.
void require_control_domain_control_matches(std::string_view canonical_bytes,
                                            const CompilerQualifiedCellIr& compiler_ir,
                                            const core::ControlDomainId& control_domain_id,
                                            std::span<const core::StopImplementationQualificationDTO>
                                                stop_implementation_qualifications = {},
                                            std::span<const core::ControlledStopSubstitutionQualificationDTO>
                                                controlled_stop_substitution_qualifications = {});

/// Require a verified artifact to agree with its retained envelope subject,
/// exact graph binding/resources, desired selection, route, and endpoints.
/// This is the read-only package-admission check used when compiler IR is no
/// longer available.
void require_control_domain_control_matches_retained_bundle(
    std::string_view canonical_bytes, const core::RetainedBundleDTO& retained_bundle,
    const core::ControlDomainId& control_domain_id,
    std::span<const QualifiedStopEvidencePolicy> stop_policies = {},
    std::span<const QualifiedControllerTopology> controller_topologies = {},
    std::span<const QualifiedProviderManifest> provider_manifests = {});

void require_control_domain_control_matches_retained_bundle(
    const ControlDomainControlArtifactClosure& artifacts,
    const core::RetainedBundleDTO& retained_bundle,
    const core::ControlDomainId& control_domain_id,
    std::span<const QualifiedStopEvidencePolicy> stop_policies = {},
    std::span<const QualifiedControllerTopology> controller_topologies = {},
    std::span<const QualifiedProviderManifest> provider_manifests = {});

/// Canonical declarative subset shared by the compiler projection and the
/// independently requalified durable catalog comparison.
[[nodiscard]] CanonicalValue stop_policy_applicability_projection(
    const core::StopEvidencePolicyDTO& policy);

}  // namespace detail
}  // namespace manyforge::deployment
