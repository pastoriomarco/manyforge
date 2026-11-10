#pragma once

#include <manyforge_core/types/program_binding.hpp>

namespace manyforge::core {

/// The established bound frame key: component-local frame ID, model instance,
/// model revision and compiled name. The owner is checked separately where
/// required; contact body roles are not compared here.
[[nodiscard]] inline bool same_program_frame(
    const ProgramFrameBindingDTO& left,
    const ProgramFrameBindingDTO& right) noexcept {
  return left.reference.frame_id == right.reference.frame_id &&
         left.model_instance_id == right.model_instance_id &&
         left.model_revision == right.model_revision &&
         left.compiled_frame_name == right.compiled_frame_name;
}

}  // namespace manyforge::core
