#pragma once

#include <manyforge_core/types/program_contract.hpp>
#include <manyforge_deployment/program_binding.hpp>
#include <manyforge_deployment/program_contract.hpp>
#include <manyforge_deployment/retained_bundle.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

#include <memory>
#include <stdexcept>
#include <string_view>

namespace manyforge::deployment {

class ProgramBindingCompilerError final : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

inline constexpr std::string_view kDirectControlMotionProgramId =
    "manyforge.direct_control.motion";
inline constexpr std::string_view kDirectControlComponentRoleId =
    "controlled_component";
inline constexpr std::string_view kDirectControlTargetRoleId =
    "command_target";
inline constexpr std::string_view kDirectControlBaseFrameRoleId = "base";
inline constexpr std::string_view kDirectControlTcpFrameRoleId = "tcp";

/// Resolve one complete authored role assignment against one exact retained
/// cell and return the sealed, context-verified Program Binding.
///
/// The Program contract, RetainedBundle, and model map are sealed inputs. The
/// role assignment remains untrusted authored selection. Catalog bytes are
/// point-read only by exact key/digest from the retained closure; no path,
/// active alias, runtime state, endpoint, or fallback selector is accepted.
[[nodiscard]] std::shared_ptr<const ProgramBinding> compile_program_binding(
    const ProgramBindingContract& program_contract,
    core::ProgramRoleAssignmentDTO role_assignment,
    const RetainedBundle& retained_bundle,
    const VerifiedRetainedModelMap& retained_model_map,
    const ICatalogArtifactStore& catalog_artifact_store);

/// Compile the build-owned direct Robot Control motion contract for one exact
/// retained command target.
///
/// The contract is derived entirely from the qualified component type and
/// retained node environment. It requires trajectory execution and joint-goal
/// planning, and includes pose/Cartesian planning only when the target
/// declares those exact capabilities. The returned Program Binding is an
/// ordinary content-addressed binding; callers must register it with the same
/// physical execution engine rather than inventing a separate direct-control
/// runtime or borrowing a scenario Program Binding.
[[nodiscard]] std::shared_ptr<const ProgramBinding>
compile_direct_control_motion_program_binding(
    const core::ControllableComponentId& controllable_component_id,
    const core::CommandTargetId& command_target_id,
    const RetainedBundle& retained_bundle,
    const VerifiedRetainedModelMap& retained_model_map,
    const ICatalogArtifactStore& catalog_artifact_store);

}  // namespace manyforge::deployment
