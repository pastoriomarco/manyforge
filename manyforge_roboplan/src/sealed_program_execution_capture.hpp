#pragma once

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/program_contract.hpp>
#include <manyforge_deployment/program_binding.hpp>
#include <manyforge_deployment/program_contract.hpp>
#include <memory>

namespace manyforge::roboplan_backend::detail {

// Identity-only ingress, shared with the cold loader so injected references or
// a foreign product refuse before repository access. Grants no claim/readiness.
[[nodiscard]] core::Result<void> ValidateSealedProgramCaptureRequest(
    const core::ProgramRunExecutionCaptureDTO &capture,
    const deployment::RetainedBundle &bundle);

// Keep binding refusal before Program loading, with the same keyed identity
// predicate used by callers that already hold their sealed source pair.
[[nodiscard]] core::Result<void> ValidateSealedProgramCaptureBinding(
    const core::ProgramRunExecutionCaptureDTO &capture,
    const deployment::ProgramBinding *binding);

// Resolve only from the exact already sealed pair. The aliases retain those
// actual products; Kernel admission still owns all Run/claim/owner checks.
[[nodiscard]] core::Result<core::ProgramRunExecutionCaptureDTO>
ResolveSealedProgramCapture(
    core::ProgramRunExecutionCaptureDTO capture,
    std::shared_ptr<const deployment::ProgramBindingContract> program,
    std::shared_ptr<const deployment::ProgramBinding> binding,
    const deployment::RetainedBundle &bundle);

} // namespace manyforge::roboplan_backend::detail
