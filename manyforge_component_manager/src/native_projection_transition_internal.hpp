#pragma once

#include <span>
#include <vector>

#include <manyforge_component_manager/runtime_convergence.hpp>
#include <manyforge_core/types/projection_apply.hpp>
#include <manyforge_core/types/retained_product.hpp>

namespace manyforge::component_manager::detail {

/// Derives the complete canonical predecessor-union-candidate native
/// projection set from compiler-verified bindings plus separately observed
/// process incarnations.
///
/// `predecessor_health` is owner-issued current-active evidence. Candidate
/// epochs come only from the complete command-fenced staged-ready record.
/// Callers cannot select a provider binding, runtime unit, or epoch.
[[nodiscard]] std::vector<manyforge::core::ProjectionTransitionRowDTO>
DeriveNativeProjectionTransitionRows(
    const manyforge::core::RetainedBundleDTO &predecessor,
    const manyforge::core::RetainedBundleDTO &candidate,
    const manyforge::core::ProjectionRevisionDTO &predecessor_revision,
    std::span<const manyforge::core::ProjectionHealthDTO> predecessor_health,
    const ManagerApplyRuntimeStagedReadyDTO &candidate_staged_ready);

/// True when the verified retained bundle contains at least one exact
/// capability contract that requires a native scene-projection participant.
[[nodiscard]] bool HasNativeProjectionRequirements(
    const manyforge::core::RetainedBundleDTO &bundle) noexcept;

} // namespace manyforge::component_manager::detail
