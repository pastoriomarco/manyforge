#pragma once

#include <manyforge_core/types/program_contract.hpp>
#include <manyforge_core/types/component_type.hpp>
#include <manyforge_deployment/program_binding.hpp>
#include <manyforge_deployment/program_binding_outcome.hpp>
#include <manyforge_deployment/program_contract.hpp>
#include <manyforge_deployment/program_reference_facts.hpp>
#include <manyforge_deployment/capability_definition_codec.hpp>
#include <manyforge_deployment/io_signal_catalog.hpp>
#include <manyforge_deployment/retained_bundle.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

#include <memory>
#include <optional>
#include <span>
#include <span>
#include <stdexcept>
#include <string>
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
inline constexpr std::string_view kDirectControlBaseFrameRoleId = "base";

/// A compiled robot TCP row projected into the Robot Control descriptor.
struct DirectControlTcpFrameAssociation final {
  core::ProgramTargetReferenceDTO target;
  core::ProgramFrameReferenceDTO frame;
  std::vector<core::CollisionBodyRoleId> contact_body_role_ids;
};

struct DirectControlTcp final {
  core::ProgramRobotTcpBindingDTO binding;
  std::string display;
  /// Exact retained target/frame associations for revalidating an existing
  /// authored selector after a graph change; not command authorization.
  std::vector<core::ProgramTargetReferenceDTO> associated_targets;
  /// Exact per-target contact evidence from direct-control sealed bindings.
  std::vector<DirectControlTcpFrameAssociation> frame_associations;
  bool mounted_here{};
  bool mounted_elsewhere{};
};

using RobotTcp = core::ProgramRobotTcpBindingDTO;

/// Compile the robot's own TCPs and every tool TCP in the Deployment into
/// exact owner-qualified rows.
[[nodiscard]] std::vector<RobotTcp> robot_tcps(
    const RetainedBundle& retained_bundle,
    const VerifiedRetainedModelMap& retained_model_map,
    const core::ControllableComponentId& robot,
    const core::CommandTargetId& robot_target,
    std::span<const QualifiedCapabilityDefinition> capabilities,
    const ICatalogArtifactSource& catalog_artifact_store);

/// The row of a robot's TCP list that `entry` selects: the rule Program
/// reference validation and run-time resolution share. The entry's frame
/// must be a frame of its target, it must name one row (exactly, else by its
/// frame alone), and that row's owner must be the robot or a tool the robot
/// carries. Refusals carry the typed reasons `active_tcp_unknown`,
/// `runtime_motion_endpoint_ambiguous` and `tcp_tool_not_mounted`.
[[nodiscard]] core::Result<const RobotTcp*> select_robot_tcp(
    const VerifiedRetainedModelMap& retained_model_map,
    const core::ControllableComponentId& robot, std::span<const RobotTcp> rows,
    const core::TcpEntryReferenceDTO& entry);

/// Whether the owner of gripper `target` has no command state to report: a
/// pneumatic gripper that is display-only (`output_mode: none`) or whose
/// valves are separate mapped I/O writes (`pneumatic_io`). Its own command is
/// then a logical one that completes without any valve effect.
[[nodiscard]] bool gripper_status_unavailable(
    const core::CellGraphDTO& graph, const core::ProgramTargetReferenceDTO& target);

/// Candidate-side TCP inventory using the compiler's qualified catalog closure.
[[nodiscard]] std::vector<RobotTcp> qualified_candidate_robot_tcps(
    const core::ProgramTargetReferenceDTO& target,
    const RetainedBundle& retained_bundle,
    const VerifiedRetainedModelMap& retained_model_map,
    const ICatalogArtifactSource& catalog_artifact_store);

/// Deterministic physical command-target inventory derived from one exact
/// retained cell. Entries are build-owned direct-control Program Bindings;
/// they select physical targets without selecting an authored Program.
struct DirectControlProgramBindingEntry final {
  core::ProgramTargetReferenceDTO target;
  std::shared_ptr<const ProgramBinding> binding;
};
struct DirectControlProgramBindingInventory final {
  std::vector<DirectControlProgramBindingEntry> motion;
  std::vector<DirectControlProgramBindingEntry> gripper;
  /// Logical input bindings only; these create no physical participant.
  std::vector<std::shared_ptr<const ProgramBinding>> io;
};

/// Resolve direct Program references against one exact retained cell and
/// return the sealed, context-verified Program Binding.
///
/// The Program contract, RetainedBundle, and model map are sealed inputs. Catalog bytes are
/// point-read only by exact key/digest from the retained closure; no path,
/// active alias, runtime state, endpoint, or fallback selector is accepted.
[[nodiscard]] std::shared_ptr<const ProgramBinding>
compile_program_binding(const ProgramBindingContract &program_contract,
                        const RetainedBundle &retained_bundle,
                        const VerifiedRetainedModelMap &retained_model_map,
                        const ICatalogArtifactSource &catalog_artifact_store);

/// Exactly one of a sealed binding for the candidate cell or a named
/// non-executable result.
struct ProgramBindingOutcome final {
  std::shared_ptr<const ProgramBinding> binding;
  std::optional<ProgramNonExecutableOutcomeDTO> non_executable;
  /// Present exactly with `non_executable`.
  std::optional<ProgramResolvedReferencesDTO> resolved_references;
};

/// Resolve a direct signal against one already-qualified compiled catalog.
/// Missing or ambiguous matches fail; target/runtime provenance always comes
/// from the exact retained row, including for derived changed-outside inputs.
[[nodiscard]] core::ProgramSignalBindingDTO resolve_program_signal_binding(
    const core::ProgramSignalReferenceDTO &reference,
    std::span<const RetainedIoSignalConfiguration> signals);

/// Resolve a direct component/target pair against the retained qualified
/// component type. Missing identities are candidate-dependent; malformed or
/// ambiguous retained catalog data remains an error.
[[nodiscard]] std::optional<ProgramBindingUnresolvedReason>
probe_program_target(const core::ControllableComponentId& component_id,
                     const core::CommandTargetId& target_id,
                     const RetainedBundle& retained_bundle,
                     const ICatalogArtifactSource& catalog_artifact_store);

/// Exact retained component type, qualified through the same catalog owner
/// used by Program binding and reference checks.
[[nodiscard]] core::ComponentTypeDTO qualified_program_component_type(
    const core::ControllableComponentId& component_id,
    const RetainedBundle& retained_bundle,
    const ICatalogArtifactSource& catalog_artifact_store);

/// Qualify one global capability against the exact retained catalog and cell.
[[nodiscard]] bool probe_program_cell_capability(
    const core::ProgramCapabilityRequirementDTO& requirement,
    const RetainedBundle& retained_bundle,
    const ICatalogArtifactSource& catalog_artifact_store);

/// Probe a required capability on one explicitly addressed target using the
/// same qualified catalog, provider and command-schema resolution as the
/// Program binding compiler. A candidate-dependent absence is returned;
/// corrupt, inconsistent or ambiguous retained inputs still throw. This is a
/// resolution primitive, not a complete Program reference validator.
[[nodiscard]] std::optional<ProgramBindingUnresolvedReason>
probe_program_target_capability(
    const core::ControllableComponentId& component_id,
    const core::CommandTargetId& target_id,
    const core::ProgramCapabilityRequirementDTO& requirement,
    const RetainedBundle& retained_bundle,
    const VerifiedRetainedModelMap& retained_model_map,
    const ICatalogArtifactSource& catalog_artifact_store);

/// Qualify several capabilities for the same target with one retained catalog
/// closure. Results correspond to the input order; no state survives the call.
[[nodiscard]] std::vector<std::optional<ProgramBindingUnresolvedReason>>
probe_program_target_capabilities(
    const core::ControllableComponentId& component_id,
    const core::CommandTargetId& target_id,
    std::span<const core::ProgramCapabilityRequirementDTO> requirements,
    const RetainedBundle& retained_bundle,
    const VerifiedRetainedModelMap& retained_model_map,
    const ICatalogArtifactSource& catalog_artifact_store);

/// Bind one preserved authored Program to one candidate cell. When an exact
/// target cannot resolve due to a candidate-dependent fact, report it with
/// affected nodes and execution groups and produce no binding. Malformed
/// input, corrupt or ambiguous retained facts and unexpected failures throw.
[[nodiscard]] ProgramBindingOutcome compile_program_binding_outcome(
    const ProgramBindingContract &program_contract,
    const RetainedBundle &retained_bundle,
    const VerifiedRetainedModelMap &retained_model_map,
    const ICatalogArtifactSource &catalog_artifact_store);

/// As above, including an absent model map for an empty candidate cell. It
/// never creates a binding without the required verified model data.
[[nodiscard]] ProgramBindingOutcome compile_program_binding_outcome(
    const ProgramBindingContract &program_contract,
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

/// The robot's exact Deployment TCP inventory, sorted by owner entry
/// triple with the robot's End-effector row first when present. Equal display
/// names or compiled frames do not merge distinct owner entries. `inventory`
/// supplies other direct-control robots for the interim mounted-elsewhere
/// flag; the model map must be verified for `retained_bundle`.
[[nodiscard]] std::vector<DirectControlTcp> direct_control_motion_tcps(
    const ProgramBinding &direct_control_binding,
    const DirectControlProgramBindingInventory &inventory,
    const RetainedBundle &retained_bundle,
    const VerifiedRetainedModelMap &retained_model_map);

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
