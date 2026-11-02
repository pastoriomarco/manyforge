#pragma once

#include <manyforge_core/types/io_activation_rule.hpp>
#include <manyforge_core/types/io_blackboard.hpp>
#include <manyforge_deployment/program_binding.hpp>
#include <manyforge_deployment/program_contract.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

namespace manyforge::deployment {

struct RetainedIoPolicyProjection final {
  std::vector<core::IoActivationRulePolicyBindingDTO> activation_policies;
  std::vector<core::IoBlackboardKeyDescriptorDTO>
      referenced_deployment_blackboard_keys;
};

/// Compile the sole retained global-rule carrier. Signal-only rules need no
/// Program or Run. With no selected Program, user-key rules are returned whole
/// and inactive with untyped waiting-key names; execution_groups still require
/// the exact selected Program+Binding. Actual Program compilation checks all
/// declarations/types/initials. No projection grants installation/source
/// authority.
[[nodiscard]] core::Result<RetainedIoPolicyProjection>
compile_retained_io_policies(const ProgramBindingContract *program,
                             const ProgramBinding *binding,
                             const RetainedBundle &retained_bundle);

[[nodiscard]] core::Result<RetainedIoPolicyProjection>
compile_retained_io_policies(const ProgramBindingContract &program,
                             const ProgramBinding &binding,
                             const RetainedBundle &retained_bundle);

} // namespace manyforge::deployment
