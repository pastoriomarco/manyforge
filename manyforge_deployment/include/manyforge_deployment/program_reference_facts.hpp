#pragma once

#include <manyforge_core/types/program_binding.hpp>
#include <manyforge_core/types/program_contract.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

#include <set>
#include <string>
#include <vector>

namespace manyforge::deployment {

/// Capture the exact component frame facts bound to one Applied identity.
[[nodiscard]] CanonicalValue CaptureProgramFrameFacts(
    const core::ProgramBindingContractDTO& contract,
    const VerifiedRetainedModelMap& model_map,
    const std::string& record_digest, const std::string& binding_id,
    const std::string& program_revision, const std::string& bundle_id,
    const std::string& graph_revision);

/// Original Program references only, before derived target or TCP expansion.
[[nodiscard]] std::set<core::ControllableComponentId> referenced_program_components(
    const core::ProgramBindingContractDTO& contract);

/// Names owned by these components, including names that resolve ambiguously.
[[nodiscard]] std::set<core::ProgramFrameReferenceDTO> program_component_frame_names(
    const std::set<core::ControllableComponentId>& components,
    const VerifiedRetainedModelMap& retained_model_map);

/// Enumerate exact component-owned retained facts; callers reject ambiguity.
[[nodiscard]] std::vector<core::ProgramFrameBindingDTO> find_program_frame_matches(
    const core::ProgramFrameReferenceDTO& reference,
    const VerifiedRetainedModelMap& retained_model_map);
[[nodiscard]] std::vector<core::ProgramCollisionBodyBindingDTO>
find_program_collision_body_matches(
    const core::ProgramCollisionBodyReferenceDTO& reference,
    const VerifiedRetainedModelMap& retained_model_map);

}  // namespace manyforge::deployment
