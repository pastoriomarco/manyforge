#pragma once

#include "ros_host_composition.hpp"
#include <filesystem>

namespace manyforge::roboplan_backend::host_detail {

// Pure sealed compiler projection. Capacity is not a source/key grant.
[[nodiscard]] core::Result<std::size_t>
CompiledProgramBlackboardKeyCapacity(const ExecutionProduct &composition);

// Slow verified reads occur without composition/Kernel/health locks. Final
// install retains the actual shared composition pin through Kernel commit.
// Repeated current contexts take the cheap immutable-pointer path.
[[nodiscard]] core::Result<void> EnsureAppliedProgramBlackboardContext(
    ExecutionCompositionAccess &access,
    core::ResourceAddressedExecutionKernel &kernel,
    const std::filesystem::path &manager_state_root,
    const core::CellId &cell_id,
    const core::IoProgramBlackboardScopeDTO &scope);

} // namespace manyforge::roboplan_backend::host_detail
