#pragma once

#include <vector>

#include <manyforge_component_manager/runtime_convergence.hpp>
#include <manyforge_core/types/projection_common.hpp>
#include <manyforge_core/types/retained_product.hpp>

namespace manyforge::component_manager::detail {

/// True when the verified retained bundle contains at least one exact
/// capability contract that requires a native scene-projection participant.
[[nodiscard]] bool HasNativeProjectionRequirements(
    const manyforge::core::RetainedBundleDTO &bundle) noexcept;

/// Derive only the candidate native set for fresh Scene initialization. No
/// predecessor union, disposition, retirement or handoff row is created.
[[nodiscard]] std::vector<
    manyforge::core::ProjectionExecutionHostBindingDTO>
DeriveCandidateNativeProjectionBindings(
    const manyforge::core::RetainedBundleDTO &candidate,
    const ManagerApplyRuntimeStagedReadyDTO &candidate_staged_ready);

} // namespace manyforge::component_manager::detail
