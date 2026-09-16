#pragma once

#include <cstddef>
#include <cstdint>
#include <manyforge_core/types/retained_product.hpp>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::core {

struct ProgramBindingDTO;

inline constexpr std::string_view kProgramApiVersion = "manyforge.program.v2";
inline constexpr std::string_view kComposableCellProgramProfile = "composable-cell-v1";
inline constexpr std::size_t kMaxProgramContractRoles = 4096U;
inline constexpr std::size_t kMaxProgramRoleRequirements = 4096U;
inline constexpr std::size_t kMaxProgramTreeNodes = 100000U;
inline constexpr std::size_t kMaxProgramExecutionGroups = 4096U;
inline constexpr std::size_t kMaxProgramExecutionMemberships = kMaxProgramTreeNodes;
inline constexpr std::size_t kMaxProgramTargetEffects = kMaxProgramTreeNodes;
inline constexpr std::size_t kMaxProgramNodePolicies = kMaxProgramTreeNodes;
inline constexpr std::size_t kMaxProgramAllowedSkills = 256U;
inline constexpr std::size_t kMaxProgramSkillIdBytes = 128U;

[[nodiscard]] constexpr bool valid_program_execution_group(std::string_view value) noexcept {
  if (value.empty() || value.size() > 128U || value == "inherit" || value.front() < 'a' ||
      value.front() > 'z') {
    return false;
  }
  for (const auto c : value) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-')) {
      return false;
    }
  }
  return true;
}

struct ProgramNodeInstanceIdTag final {};
using ProgramNodeInstanceId = StrongString<ProgramNodeInstanceIdTag>;

/// A target/capability association derived from the qualified node catalog,
/// its parameter defaults/conditions and this exact Program's literal roles.
struct ProgramNodeTargetEffectDTO final {
  TargetRoleId target_role_id;
  CapabilityId capability_id;
  /// Qualified catalog fact: this requirement may issue a physical command
  /// against the bound target, rather than only read/plan/validate it.
  bool effectful{};

  friend bool operator==(const ProgramNodeTargetEffectDTO&,
                         const ProgramNodeTargetEffectDTO&) = default;
};

struct ProgramExecutionNodeDTO final {
  ProgramNodeInstanceId instance_id;
  std::string owner_api;
  std::vector<ProgramNodeTargetEffectDTO> target_effects;

  friend bool operator==(const ProgramExecutionNodeDTO&, const ProgramExecutionNodeDTO&) = default;
};

enum class ProgramExecutionPolicy {
  kIndependent,
  kCommonStart,
  kCommonStartAndFinish,
  kSyncOffsetTrajectory,
};

[[nodiscard]] constexpr std::string_view to_string(const ProgramExecutionPolicy value) noexcept {
  switch (value) {
    case ProgramExecutionPolicy::kIndependent:
      return "independent";
    case ProgramExecutionPolicy::kCommonStart:
      return "common_start";
    case ProgramExecutionPolicy::kCommonStartAndFinish:
      return "common_start_and_finish";
    case ProgramExecutionPolicy::kSyncOffsetTrajectory:
      return "sync_offset_traj";
  }
  return "independent";
}

struct ProgramExecutionSelectionDTO final {
  std::string group_id;
  // Sorted indices into ProgramExecutionTableDTO::nodes; names are stored once.
  std::vector<std::uint32_t> node_indices;
  /// One authored execution policy. UI6-I qualifies kIndependent; later
  /// synchronized policies remain explicit and fail before effects until F2.
  ProgramExecutionPolicy execution_policy{ProgramExecutionPolicy::kIndependent};

  friend bool operator==(const ProgramExecutionSelectionDTO&,
                         const ProgramExecutionSelectionDTO&) = default;
};

/// Immutable derived data, not another artifact or runtime group state.
/// The sealed Program contract proves its provenance. All authored nodes,
/// including logical and never-started nodes, occur in instance-ID order.
/// The built-in `all` selector means the complete nodes vector; it is not
/// redundantly stored in named_selections or in sparse policy rows.
struct ProgramExecutionTableDTO final {
  std::vector<ProgramExecutionNodeDTO> nodes;
  std::vector<ProgramExecutionSelectionDTO> named_selections;

  friend bool operator==(const ProgramExecutionTableDTO&,
                         const ProgramExecutionTableDTO&) = default;
};

/// Original authored work attribution, separate from diagnostic ActionSpan
/// labels (which may name derived phases). A new attempt never changes scope.
struct ProgramNodeExecutionScopeDTO final {
  ProgramNodeInstanceId node_instance_id;
  std::string node_attempt_id;

  friend bool operator==(const ProgramNodeExecutionScopeDTO&,
                         const ProgramNodeExecutionScopeDTO&) = default;
};

[[nodiscard]] inline bool valid_program_node_execution_scope(
    const ProgramNodeExecutionScopeDTO& scope) noexcept {
  return !scope.node_instance_id.empty() && scope.node_instance_id.value().size() <= 128U &&
         scope.node_instance_id.value().find('\0') == std::string::npos &&
         !scope.node_attempt_id.empty() && scope.node_attempt_id.size() <= 512U &&
         scope.node_attempt_id.find('\0') == std::string::npos;
}

/// Process-local input from the sealed Program/Binding composition boundary.
/// Transports carry the identities only, never a caller-supplied table.
/// Kernel installation independently joins current claim/owner/configuration.
struct ProgramRunExecutionCaptureDTO final {
  RequestOwnerEpoch request_owner_epoch;
  RequestOwnerEpoch program_runner_owner_epoch;
  CellAdmissionClaimGeneration claim_generation;
  ProgramRunId program_run_id;
  ProgramId program_id;
  ProgramRevision program_revision;
  ProgramBindingId program_binding_id;
  RetainedBundleId retained_bundle_id;
  GraphRevision graph_revision;
  std::shared_ptr<const ProgramExecutionTableDTO> table;
  /// Alias of the same verified Binding used by the composition boundary.
  /// Required at Kernel installation; public registration carries IDs only
  /// and cannot populate either this reference or the execution table.
  std::shared_ptr<const ProgramBindingDTO> binding{};
};

enum class ProgramModificationPolicyValue : std::uint8_t {
  kForbidden = 0,
  kHumanApproval,
  kLlmAllowed,
};

[[nodiscard]] constexpr std::string_view to_string(ProgramModificationPolicyValue value) noexcept {
  switch (value) {
    case ProgramModificationPolicyValue::kForbidden:
      return "forbidden";
    case ProgramModificationPolicyValue::kHumanApproval:
      return "human_approval";
    case ProgramModificationPolicyValue::kLlmAllowed:
      return "llm_allowed";
  }
  return {};
}

enum class ProgramInterventionDelegate : std::uint8_t {
  kForbidden = 0,
  kHumanOnly,
  kLlmAllowed,
  kProgramOnly,
};

[[nodiscard]] constexpr std::string_view to_string(ProgramInterventionDelegate value) noexcept {
  switch (value) {
    case ProgramInterventionDelegate::kForbidden:
      return "forbidden";
    case ProgramInterventionDelegate::kHumanOnly:
      return "human_only";
    case ProgramInterventionDelegate::kLlmAllowed:
      return "llm_allowed";
    case ProgramInterventionDelegate::kProgramOnly:
      return "program_only";
  }
  return {};
}

enum class ProgramRecoveryAction : std::uint8_t {
  kIntervention = 0,
  kRuntimeOverride,
};

[[nodiscard]] constexpr std::string_view to_string(ProgramRecoveryAction value) noexcept {
  switch (value) {
    case ProgramRecoveryAction::kIntervention:
      return "intervention";
    case ProgramRecoveryAction::kRuntimeOverride:
      return "runtime_override";
  }
  return {};
}

enum class ProgramRecoveryPersistenceCeiling : std::uint8_t {
  kSessionOnly = 0,
  kRuntimeOverride,
  kDraftPatch,
};

[[nodiscard]] constexpr std::string_view to_string(
    ProgramRecoveryPersistenceCeiling value) noexcept {
  switch (value) {
    case ProgramRecoveryPersistenceCeiling::kSessionOnly:
      return "session_only";
    case ProgramRecoveryPersistenceCeiling::kRuntimeOverride:
      return "runtime_override";
    case ProgramRecoveryPersistenceCeiling::kDraftPatch:
      return "draft_patch";
  }
  return {};
}

enum class ProgramOperatorStopPolicy : std::uint8_t {
  kInherit = 0,
  kHold,
  kContinue,
};

[[nodiscard]] constexpr std::string_view to_string(ProgramOperatorStopPolicy value) noexcept {
  switch (value) {
    case ProgramOperatorStopPolicy::kInherit:
      return "inherit";
    case ProgramOperatorStopPolicy::kHold:
      return "hold";
    case ProgramOperatorStopPolicy::kContinue:
      return "continue";
  }
  return {};
}

struct ProgramNodeModificationPolicyDTO final {
  ProgramModificationPolicyValue runtime_override{ProgramModificationPolicyValue::kForbidden};
  ProgramModificationPolicyValue persistent_edit{ProgramModificationPolicyValue::kForbidden};
  ProgramInterventionDelegate intervention_delegate{ProgramInterventionDelegate::kForbidden};

  friend bool operator==(const ProgramNodeModificationPolicyDTO&,
                         const ProgramNodeModificationPolicyDTO&) = default;
};

struct ProgramNodeRecoveryPolicyDTO final {
  bool allow_intervention{false};
  std::vector<ProgramRecoveryAction> allowed_recovery_actions;
  std::uint8_t max_retries{0U};
  std::vector<std::string> allowed_skills;
  ProgramRecoveryPersistenceCeiling persistence_ceiling{
      ProgramRecoveryPersistenceCeiling::kSessionOnly};

  friend bool operator==(const ProgramNodeRecoveryPolicyDTO&,
                         const ProgramNodeRecoveryPolicyDTO&) = default;
};

/// Complete resolved authored policy values for one non-default Program node.
///
/// This DTO is shared by the verified Program contract and its compiled
/// Program-Binding. It intentionally contains no runtime state, node kind, or
/// browser/editor metadata.
struct ProgramNodePolicyDTO final {
  ProgramNodeInstanceId instance_id;
  ProgramNodeModificationPolicyDTO modification_policy;
  ProgramNodeRecoveryPolicyDTO recovery_policy;
  ProgramOperatorStopPolicy operator_stop_policy{ProgramOperatorStopPolicy::kInherit};

  friend bool operator==(const ProgramNodePolicyDTO&, const ProgramNodePolicyDTO&) = default;
};

enum class ProgramCapabilityDisposition : std::uint8_t {
  kRequired = 0,
  kOptional,
};

[[nodiscard]] constexpr std::string_view to_string(ProgramCapabilityDisposition value) noexcept {
  switch (value) {
    case ProgramCapabilityDisposition::kRequired:
      return "required";
    case ProgramCapabilityDisposition::kOptional:
      return "optional";
  }
  return {};
}

struct ProgramComponentRoleRequirementDTO final {
  ComponentRoleId component_role_id;
  std::vector<std::string> required_component_type_tags;
  std::vector<ModelRoleId> required_model_roles;
  std::vector<AttachmentInterfaceId> required_attachment_interfaces;

  friend bool operator==(const ProgramComponentRoleRequirementDTO&,
                         const ProgramComponentRoleRequirementDTO&) = default;
};

struct ProgramCapabilityRequirementDTO final {
  CapabilityId capability_id;
  CapabilityDefinitionRevision capability_definition_revision;
  ProgramCapabilityDisposition disposition;

  friend bool operator==(const ProgramCapabilityRequirementDTO&,
                         const ProgramCapabilityRequirementDTO&) = default;
};

struct ProgramTargetRoleRequirementDTO final {
  TargetRoleId target_role_id;
  ComponentRoleId component_role_id;
  std::vector<ProgramCapabilityRequirementDTO> required_capabilities;
  std::vector<FrameRoleId> required_frame_roles;
  std::vector<ResourceRoleId> required_resource_roles;
  std::vector<CollisionBodyRoleId> required_collision_body_roles;
  std::vector<ModelRoleId> required_model_roles;

  friend bool operator==(const ProgramTargetRoleRequirementDTO&,
                         const ProgramTargetRoleRequirementDTO&) = default;
};

/// Portable component-model side of one Program-owned contact-grasp pair.
/// The selected object remains a typed runtime-object binding until operation
/// admission; generated collision-body names are never authored here.
struct ProgramContactGraspBodySelectorDTO final {
  ComponentRoleId component_role_id;
  std::optional<ModelRoleId> model_role_id;
  CollisionBodyRoleId collision_body_role_id;

  friend bool operator==(const ProgramContactGraspBodySelectorDTO&,
                         const ProgramContactGraspBodySelectorDTO&) = default;
};

/// Binding-relevant projection of the currently qualified geometry-only
/// `contact_grasp` policy subset. The complete canonical policy remains in the
/// sealed Program; this value contains only what the binding compiler must
/// resolve before a selected scene resource is known.
struct ProgramContactGraspTemplateDTO final {
  ContactPolicyId contact_policy_id;
  ContactPolicyRevision contact_policy_revision;
  RuntimeObjectId runtime_object_id;
  std::vector<ProgramContactGraspBodySelectorDTO> allowed_body_selectors;
  double max_penetration_m{};
  double terminal_window_start_fraction{};

  friend bool operator==(const ProgramContactGraspTemplateDTO&,
                         const ProgramContactGraspTemplateDTO&) = default;
};

/// Binding-relevant projection of one complete, revision-verified Program.
///
/// This DTO remains an untrusted representation. The deployment package's
/// sealed ProgramBindingContract is the proof that it was extracted from the
/// exact canonical Program and resolved against one retained node catalog.
struct ProgramBindingContractDTO final {
  ProgramId program_id;
  ProgramRevision program_revision;
  RetainedNodeEnvironmentDTO node_environment;
  std::vector<ProgramComponentRoleRequirementDTO> component_roles;
  std::vector<ProgramTargetRoleRequirementDTO> target_roles;
  std::vector<ProgramContactGraspTemplateDTO> contact_grasp_templates{};
  std::vector<ProgramNodePolicyDTO> node_policies{};

  friend bool operator==(const ProgramBindingContractDTO&,
                         const ProgramBindingContractDTO&) = default;
};

struct ProgramComponentRoleAssignmentDTO final {
  ComponentRoleId component_role_id;
  ControllableComponentId controllable_component_id;

  friend bool operator==(const ProgramComponentRoleAssignmentDTO&,
                         const ProgramComponentRoleAssignmentDTO&) = default;
};

struct ProgramTargetRoleAssignmentDTO final {
  TargetRoleId target_role_id;
  CommandTargetId command_target_id;

  friend bool operator==(const ProgramTargetRoleAssignmentDTO&,
                         const ProgramTargetRoleAssignmentDTO&) = default;
};

struct ProgramRoleAssignmentDTO final {
  std::vector<ProgramComponentRoleAssignmentDTO> component_roles;
  std::vector<ProgramTargetRoleAssignmentDTO> target_roles;

  friend bool operator==(const ProgramRoleAssignmentDTO&,
                         const ProgramRoleAssignmentDTO&) = default;
};

}  // namespace manyforge::core
