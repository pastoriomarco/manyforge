#pragma once

#include <manyforge_deployment/canonical_value.hpp>

#include "prepared_activation_materialization_internal.hpp"

namespace manyforge::deployment::detail {

[[nodiscard]] CanonicalValue prepared_target_default_tcp(
    const VerifiedRetainedModelMap& model,
    const core::ProgramTargetReferenceDTO& target);

// Pure projection of one immutable prepared candidate. Catalog and model
// qualification is shared with the Program reference validator/compiler.
[[nodiscard]] CanonicalValue prepared_selector_facts(
    const PreparedActivationMaterialization& candidate);

}  // namespace manyforge::deployment::detail
