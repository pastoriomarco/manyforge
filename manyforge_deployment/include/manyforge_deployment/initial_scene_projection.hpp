#pragma once

#include <vector>

#include <manyforge_core/types/scene_transaction.hpp>
#include <manyforge_deployment/program_binding.hpp>
#include <manyforge_deployment/program_contract.hpp>

namespace manyforge::deployment {

namespace detail {
/// Pure projection used by the sealed overload and its focused schema tests.
[[nodiscard]] std::vector<core::AddObjectRequestDTO>
project_bound_initial_scene_value(const CanonicalValue &normalized_program,
                                  const core::ProgramBindingDTO &binding,
                                  const core::CellId &cell_id);
} // namespace detail

/// Project the immutable Program's authored initial Scene through its exact
/// binding. These are intents, not an authoritative Scene snapshot: only the
/// Scene owner can resolve poses, validate geometry and mint a revision.
[[nodiscard]] std::vector<core::AddObjectRequestDTO>
project_bound_initial_scene(const ProgramBindingContract &program,
                            const ProgramBinding &binding,
                            const core::CellId &cell_id);

} // namespace manyforge::deployment
