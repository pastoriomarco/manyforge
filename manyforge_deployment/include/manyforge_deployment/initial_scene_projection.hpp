#pragma once

#include <vector>

#include <manyforge_core/types/scene_transaction.hpp>
#include <manyforge_deployment/program_binding.hpp>
#include <manyforge_deployment/program_binding_outcome.hpp>
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

/// Project the authored initial Scene of a Program that is non-executable in
/// its candidate cell (H1a), through the roles that did resolve against that
/// candidate. Cell-frame objects are projected exactly. An object on a
/// component frame is placed only when its component role, model role and
/// frame all resolved against the verified candidate (for example a surviving
/// arm after its gripper target was removed); otherwise it is refused by name
/// rather than dropped, so an authored obstacle never silently disappears. The
/// results remain intents: the Scene owner resolves and validates every pose.
[[nodiscard]] std::vector<core::AddObjectRequestDTO>
project_resolved_initial_scene(const ProgramBindingContract &program,
                               const ProgramResolvedRolesDTO &resolved_roles,
                               const core::CellId &cell_id);

} // namespace manyforge::deployment
