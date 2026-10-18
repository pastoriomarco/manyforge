#pragma once

#include <manyforge_core/types/program_contract.hpp>
#include <manyforge_deployment/program_binding.hpp>
#include <manyforge_deployment/program_binding_outcome.hpp>
#include <manyforge_deployment/program_contract.hpp>
#include <manyforge_deployment/retained_bundle.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace manyforge::deployment {

class ProgramBindingCompilerError final : public std::invalid_argument {
public:
  using std::invalid_argument::invalid_argument;
};

inline constexpr std::string_view kDirectControlMotionProgramId =
    "manyforge.direct_control.motion";
inline constexpr std::string_view kDirectControlGripperProgramId =
    "manyforge.direct_control.gripper";
inline constexpr std::string_view kDirectControlIoProgramId =
    "manyforge.direct_control.io";
inline constexpr std::string_view kDirectControlComponentRoleId =
    "controlled_component";
inline constexpr std::string_view kDirectControlTargetRoleId = "command_target";
inline constexpr std::string_view kDirectControlBaseFrameRoleId = "base";
inline constexpr std::string_view kDirectControlTcpFrameRoleId = "tcp";

/// Deterministic physical command-target inventory derived from one exact
/// retained cell. Entries are build-owned direct-control Program Bindings;
/// they select physical targets without selecting an authored Program.
struct DirectControlProgramBindingInventory final {
  std::vector<std::shared_ptr<const ProgramBinding>> motion;
  std::vector<std::shared_ptr<const ProgramBinding>> gripper;
  /// Logical input bindings only; these create no physical participant.
  std::vector<std::shared_ptr<const ProgramBinding>> io;
};

/// Resolve one complete authored role assignment against one exact retained
/// cell and return the sealed, context-verified Program Binding.
///
/// The Program contract, RetainedBundle, and model map are sealed inputs. The
/// role assignment remains untrusted authored selection. Catalog bytes are
/// point-read only by exact key/digest from the retained closure; no path,
/// active alias, runtime state, endpoint, or fallback selector is accepted.
[[nodiscard]] std::shared_ptr<const ProgramBinding>
compile_program_binding(const ProgramBindingContract &program_contract,
                        core::ProgramRoleAssignmentDTO role_assignment,
                        const RetainedBundle &retained_bundle,
                        const VerifiedRetainedModelMap &retained_model_map,
                        const ICatalogArtifactSource &catalog_artifact_store);

/// Exactly one of a sealed binding for the candidate cell or a named
/// non-executable result.
struct ProgramBindingOutcome final {
  std::shared_ptr<const ProgramBinding> binding;
  std::optional<ProgramNonExecutableOutcomeDTO> non_executable;
  /// Present exactly with `non_executable`.
  std::optional<ProgramResolvedRolesDTO> resolved_roles;
};

/// Bind one preserved authored Program to one candidate cell (H1a).
///
/// Identical to `compile_program_binding` when every role resolves. When a
/// role does not resolve only because of candidate-dependent facts (a missing
/// component or target, an incompatible type, an unavailable capability),
/// every such role is reported with the Program nodes and execution groups it
/// affects, and no binding is produced. Malformed input, corrupt or ambiguous
/// retained facts and unexpected failures still throw
/// ProgramBindingCompilerError: they never become a non-executable Program.
[[nodiscard]] ProgramBindingOutcome compile_program_binding_outcome(
    const ProgramBindingContract &program_contract,
    core::ProgramRoleAssignmentDTO role_assignment,
    const RetainedBundle &retained_bundle,
    const VerifiedRetainedModelMap &retained_model_map,
    const ICatalogArtifactSource &catalog_artifact_store);

/// As above, with no model map for an empty candidate cell. This overload
/// can only return a named non-executable outcome when all assigned component
/// roles are absent; it never creates a binding without verified model data.
[[nodiscard]] ProgramBindingOutcome compile_program_binding_outcome(
    const ProgramBindingContract &program_contract,
    core::ProgramRoleAssignmentDTO role_assignment,
    const RetainedBundle &retained_bundle,
    const std::optional<VerifiedRetainedModelMap> &retained_model_map,
    const ICatalogArtifactSource &catalog_artifact_store);

/// Compile the build-owned direct Robot Control motion contract for one exact
/// retained command target.
///
/// The contract is derived entirely from the qualified component type and
/// retained node environment. It requires trajectory execution and joint-goal
/// planning and retains every other qualified capability declared by the same
/// physical target. The complete target closure lets lifecycle composition
/// project one exact representative per selected control binding while all of
/// those representatives remain registered with the same physical execution
/// engine. The returned Program Binding is an ordinary content-addressed
/// binding; callers must not invent a separate direct-control runtime or
/// borrow a scenario Program Binding.
[[nodiscard]] std::shared_ptr<const ProgramBinding>
compile_direct_control_motion_program_binding(
    const core::ControllableComponentId &controllable_component_id,
    const core::CommandTargetId &command_target_id,
    const RetainedBundle &retained_bundle,
    const VerifiedRetainedModelMap &retained_model_map,
    const ICatalogArtifactSource &catalog_artifact_store);

/// Compile the build-owned direct Robot Control gripper contract for one exact
/// retained command target.
///
/// The binding uses the target's qualified command schema and complete
/// model/resource/control capability closure. It is an ordinary Program
/// Binding whose control-binding representatives may share an existing
/// physical gripper engine only after exact owner equality is verified.
[[nodiscard]] std::shared_ptr<const ProgramBinding>
compile_direct_control_gripper_program_binding(
    const core::ControllableComponentId &controllable_component_id,
    const core::CommandTargetId &command_target_id,
    const RetainedBundle &retained_bundle,
    const VerifiedRetainedModelMap &retained_model_map,
    const ICatalogArtifactSource &catalog_artifact_store);

/// Compile a logical input-read binding from the exact retained input roles and
/// qualified io.gpio.get_input offer. It carries no motion/control resources
/// and must not be used to fabricate a physical execution participant.
[[nodiscard]] std::shared_ptr<const ProgramBinding>
compile_direct_control_io_program_binding(
    const core::ControllableComponentId &controllable_component_id,
    const core::CommandTargetId &command_target_id,
    const RetainedBundle &retained_bundle,
    const VerifiedRetainedModelMap &retained_model_map,
    const ICatalogArtifactSource &catalog_artifact_store);

/// Compile every complete direct-control target in deterministic retained-cell
/// order. A target is included only when its qualified component type declares
/// the complete required capability family; a declared family with an invalid
/// model/resource/control closure fails rather than being silently skipped.
[[nodiscard]] DirectControlProgramBindingInventory
compile_direct_control_program_binding_inventory(
    const RetainedBundle &retained_bundle,
    const VerifiedRetainedModelMap &retained_model_map,
    const ICatalogArtifactSource &catalog_artifact_store);

} // namespace manyforge::deployment
