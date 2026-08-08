#pragma once

#include <manyforge_core/types/retained_product.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::core {

inline constexpr std::string_view kProgramApiVersion = "manyforge.program.v1";
inline constexpr std::string_view kComposableCellProgramProfile =
    "composable-cell-v1";
inline constexpr std::size_t kMaxProgramContractRoles = 4096U;
inline constexpr std::size_t kMaxProgramRoleRequirements = 4096U;

enum class ProgramCapabilityDisposition : std::uint8_t {
  kRequired = 0,
  kOptional,
};

[[nodiscard]] constexpr std::string_view to_string(
    ProgramCapabilityDisposition value) noexcept {
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
