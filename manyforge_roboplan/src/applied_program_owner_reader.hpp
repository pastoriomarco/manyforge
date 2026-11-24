#pragma once

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/program_blackboard_context.hpp>
#include <manyforge_core/types/program_contract.hpp>
#include <manyforge_deployment/program_binding.hpp>
#include <manyforge_deployment/program_contract.hpp>

namespace manyforge::roboplan_backend::detail {

/// The installed product's Applied Program, once its Apply's claim release
/// stated that the Apply succeeded. It grants no installation by itself.
struct AppliedProgramOwnerSnapshot final {
  core::ActiveBundlePublicationGeneration publication_generation;
  core::ProgramRevision program_revision;
  core::ProgramBindingId program_binding_id;
};

/// Pure sealed-product join. No filesystem/owner callback is retained. The
/// caller must still perform the actual final composition/admission/service/
/// currentness install fence.
[[nodiscard]] core::Result<core::AppliedProgramBlackboardContextDTO>
CompileAppliedProgramBlackboardContext(
    const AppliedProgramOwnerSnapshot &applied,
    std::shared_ptr<const deployment::ProgramBindingContract> program,
    std::shared_ptr<const deployment::ProgramBinding> binding,
    const deployment::RetainedBundle &bundle);

} // namespace manyforge::roboplan_backend::detail
