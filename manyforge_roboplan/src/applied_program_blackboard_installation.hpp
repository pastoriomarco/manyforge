#pragma once

#include "ros_host_composition.hpp"
#include <functional>
#include <optional>

namespace manyforge::roboplan_backend::host_detail {

/// Whether an Apply succeeded (true), failed (false) or is still pending
/// (nullopt), as the Manager's release of its claim stated it.
using ApplyOutcomeLookup = std::function<std::optional<bool>(
    const core::NamedApplyPublicationTransactionDTO &)>;

// Pure sealed compiler projection. Capacity is not a source/key grant.
[[nodiscard]] core::Result<std::size_t>
CompiledProgramBlackboardKeyCapacity(const ExecutionProduct &composition);

// Installs the context once the product's Apply is known succeeded; answers
// the retry hint while it is pending. Final install retains the actual shared
// composition pin through Kernel commit. Repeated current contexts take the
// cheap immutable-pointer path.
[[nodiscard]] core::Result<void> EnsureAppliedProgramBlackboardContext(
    ExecutionCompositionAccess &access,
    core::ResourceAddressedExecutionKernel &kernel,
    const ApplyOutcomeLookup &apply_outcome,
    const core::IoProgramBlackboardScopeDTO &scope);

} // namespace manyforge::roboplan_backend::host_detail
