#pragma once

#include <filesystem>
#include <manyforge_core/types/active_bundle_publication.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_deployment/program_binding.hpp>
#include <manyforge_deployment/prepared_activation_input.hpp>
#include <manyforge_deployment/program_contract.hpp>

namespace manyforge::roboplan_backend::detail {

struct SelectedProgramProducts final {
  std::shared_ptr<const deployment::ProgramBindingContract> program;
  std::shared_ptr<const deployment::ProgramBinding> binding;
  deployment::PreparedActivationProgramOutcomeDTO outcome{};
};

// Read the existing original NamedApply's prepared selection, including before
// terminal publication. These sealed prospective products grant no Applied,
// current owner, fence, source installation or execution authority.
[[nodiscard]] core::Result<SelectedProgramProducts> ReadSelectedProgramProducts(
    const std::filesystem::path &manager_state_root,
    const core::CellId &cell_id,
    const core::NamedApplyPublicationTransactionDTO &origin,
    const deployment::RetainedBundle &bundle,
    const deployment::VerifiedRetainedModelMap &model_map,
    const std::filesystem::path &package_root,
    const std::filesystem::path &program_artifact_root);

// A genuinely map-free no-Program selection still returns its actual parsed
// outcome; an executable binding always requires the real verified map.
[[nodiscard]] core::Result<SelectedProgramProducts> ReadSelectedProgramProducts(
    const std::filesystem::path &manager_state_root,
    const core::CellId &cell_id,
    const core::NamedApplyPublicationTransactionDTO &origin,
    const deployment::RetainedBundle &bundle,
    const deployment::VerifiedRetainedModelMap *model_map,
    const std::filesystem::path &package_root,
    const std::filesystem::path &program_artifact_root);

} // namespace manyforge::roboplan_backend::detail
