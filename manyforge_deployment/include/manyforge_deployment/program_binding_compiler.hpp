#pragma once

#include <manyforge_core/types/program_contract.hpp>
#include <manyforge_deployment/program_binding.hpp>
#include <manyforge_deployment/program_contract.hpp>
#include <manyforge_deployment/retained_bundle.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

#include <memory>
#include <stdexcept>

namespace manyforge::deployment {

class ProgramBindingCompilerError final : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

/// Resolve one complete authored role assignment against one exact retained
/// cell and return the sealed, context-verified Program Binding.
///
/// The Program contract, RetainedBundle, and model map are sealed inputs. The
/// role assignment remains untrusted authored selection. Catalog bytes are
/// point-read only by exact key/digest from the retained closure; no path,
/// active alias, runtime state, endpoint, or fallback selector is accepted.
[[nodiscard]] std::shared_ptr<const ProgramBinding> compile_program_binding(
    const ProgramBindingContract& program_contract,
    core::ProgramRoleAssignmentDTO role_assignment,
    const RetainedBundle& retained_bundle,
    const VerifiedRetainedModelMap& retained_model_map,
    const ICatalogArtifactStore& catalog_artifact_store);

}  // namespace manyforge::deployment
