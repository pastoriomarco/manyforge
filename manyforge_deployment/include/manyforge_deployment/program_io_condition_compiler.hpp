#pragma once

#include <manyforge_core/types/program_io_condition.hpp>
#include <manyforge_deployment/program_binding.hpp>
#include <manyforge_deployment/program_contract.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

namespace manyforge::deployment {

/// Lower the actual literal node conditions from one exact sealed Program and
/// Binding. Signal operands resolve only through that Binding's exact retained
/// signal references; key operands resolve only through that Program's scalar declarations.
/// The projection preserves each node's own desired-trip selection metadata;
/// it selects no Deployment policy and grants no prepared Stop/runtime scope.
/// It cannot substitute for the complete Program plus Deployment reference union.
[[nodiscard]] core::Result<core::ProgramIoConditionProjectionDTO>
compile_program_io_conditions(const ProgramBindingContract &program,
                              const ProgramBinding &binding,
                              const RetainedBundle &retained_bundle);

} // namespace manyforge::deployment
