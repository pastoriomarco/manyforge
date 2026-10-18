#pragma once

#include <manyforge_core/types/io_rule_stop_response.hpp>
#include <manyforge_deployment/prepared_activation_input.hpp>
#include <span>

namespace manyforge::deployment {

/// Exact raw dependencies needed to validate authored node Stop selections at
/// Apply. Reads only sealed Program/Binding/bundle identities; it does not
/// resolve an active configuration, prepare a runtime plan or grant a scope.
[[nodiscard]] core::Result<std::vector<core::RetainedArtifactRefDTO>>
required_program_io_stop_artifacts(const ProgramBindingContract &program,
                                   const ProgramBinding &binding,
                                   const RetainedBundle &bundle);

/// Exact retained artifact dependency for one real control-binding subject.
[[nodiscard]] core::Result<core::RetainedArtifactRefDTO>
required_io_stop_artifact_for_binding(
    const RetainedBundle &bundle, const core::ControlBindingId &binding_id,
    const core::ControlBindingRevision &binding_revision);

/// Shared qualified static chain for node/global Apply checks. The caller
/// selects the sealed route; Core performs entry/floor qualification.
[[nodiscard]] core::Result<std::vector<core::IoRuleStopEntryRowDTO>>
read_qualified_io_stop_stage_rows(
    const RetainedBundle &bundle, const VerifiedRetainedModelMap &model_map,
    const core::ControlBindingId &binding_id,
    const core::ControlBindingRevision &binding_revision,
    const core::ControlRouteId &route_id,
    std::span<const PreparedActivationArtifact> raw_artifacts);

/// Read-only stage IDs in the exact qualified retained binding/route order.
/// This is declarative metadata, not current readiness or Stop permission.
[[nodiscard]] core::Result<CanonicalValue>
read_qualified_io_stop_response_metadata(
    const RetainedBundle &bundle, const VerifiedRetainedModelMap &model_map,
    const core::ControlBindingId &binding_id,
    const core::ControlBindingRevision &binding_revision,
    const core::ControlRouteId &route_id,
    const IVerifiedRetainedProductArtifactSource &artifact_source);

/// Reuses the same qualified static control-product and Core metadata checks
/// before emitting and when independently recompiling prepared activation.
/// Supplied bytes must match exact retained product references and the sealed
/// model map. No runtime owner/generation or prepared plan is synthesized.
[[nodiscard]] core::Result<void> validate_program_io_stop_responses(
    const ProgramBindingContract &program, const ProgramBinding &binding,
    const RetainedBundle &bundle, const VerifiedRetainedModelMap &model_map,
    std::span<const PreparedActivationArtifact> raw_artifacts);

} // namespace manyforge::deployment
