#pragma once

#include <filesystem>
#include <manyforge_component_manager/selected_runtime_composition.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_deployment/program_binding.hpp>
#include <manyforge_deployment/program_contract.hpp>
#include <optional>

namespace manyforge::roboplan_backend::detail {

struct SelectedProgramProducts final {
  std::shared_ptr<const deployment::ProgramBindingContract> program;
  std::shared_ptr<const deployment::ProgramBinding> binding;
};

// Loads the sealed products of the Program the Manager's Apply request binds;
// no bound Program yields none. Grants no Applied or execution authority.
[[nodiscard]] core::Result<SelectedProgramProducts> LoadSelectedProgramProducts(
    const std::optional<component_manager::HostApplyProgramSelection> &program,
    const deployment::RetainedBundle &bundle,
    const deployment::VerifiedRetainedModelMap &model_map,
    const std::filesystem::path &package_root,
    const std::filesystem::path &program_artifact_root);

// A bound Program always requires the verified model map.
[[nodiscard]] core::Result<SelectedProgramProducts> LoadSelectedProgramProducts(
    const std::optional<component_manager::HostApplyProgramSelection> &program,
    const deployment::RetainedBundle &bundle,
    const deployment::VerifiedRetainedModelMap *model_map,
    const std::filesystem::path &package_root,
    const std::filesystem::path &program_artifact_root);

} // namespace manyforge::roboplan_backend::detail
