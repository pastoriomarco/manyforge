#pragma once

#include <manyforge_core/types/program_binding.hpp>

namespace manyforge::core {

/// The physical frame identity of a bound Program frame: its role, model
/// instance, model revision and compiled frame name. The role keeps rows
/// exact where two roles share one frame (a UR5's `flange` and `tcp`).
/// Contact body roles belong to a Program's view and are not compared.
[[nodiscard]] inline bool same_program_frame(
    const ProgramFrameRoleBindingDTO& left,
    const ProgramFrameRoleBindingDTO& right) noexcept {
  return left.frame_role_id == right.frame_role_id &&
         left.model_instance_id == right.model_instance_id &&
         left.model_revision == right.model_revision &&
         left.compiled_frame_name == right.compiled_frame_name;
}

}  // namespace manyforge::core
