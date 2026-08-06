#pragma once

#include <manyforge_core/types/retained_product.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::core {

inline constexpr std::string_view kProgramBindingApiVersion =
    "manyforge.program_binding.v1";
inline constexpr std::size_t kMaxProgramRoleBindings = 4096U;
inline constexpr std::size_t kMaxProgramNestedRoleBindings = 4096U;
inline constexpr std::size_t kMaxProgramCapabilityBindings = 4096U;
inline constexpr std::size_t kMaxProgramBindingProvenanceRecords = 16384U;

struct ProgramModelRoleBindingDTO final {
  ModelRoleId model_role_id;
  ModelInstanceId model_instance_id;
  ModelRevision model_revision;

  friend bool operator==(const ProgramModelRoleBindingDTO&,
                         const ProgramModelRoleBindingDTO&) = default;
};

struct ProgramComponentRoleBindingDTO final {
  ComponentRoleId component_role_id;
  ControllableComponentId controllable_component_id;
  ComponentTypeId component_type_id;
  ComponentTypeRevision component_type_revision;
  std::vector<ProgramModelRoleBindingDTO> model_role_bindings;

  friend bool operator==(const ProgramComponentRoleBindingDTO&,
                         const ProgramComponentRoleBindingDTO&) = default;
};

struct ProgramFrameRoleBindingDTO final {
  FrameRoleId frame_role_id;
  ModelInstanceId model_instance_id;
  ModelRevision model_revision;
  std::string compiled_frame_name;

  friend bool operator==(const ProgramFrameRoleBindingDTO&,
                         const ProgramFrameRoleBindingDTO&) = default;
};

struct ProgramResourceRoleBindingDTO final {
  ResourceRoleId resource_role_id;
  std::vector<ResourceId> resource_ids;
  std::vector<std::string> compiled_resource_names;

  friend bool operator==(const ProgramResourceRoleBindingDTO&,
                         const ProgramResourceRoleBindingDTO&) = default;
};

struct ProgramCollisionBodyRoleBindingDTO final {
  CollisionBodyRoleId collision_body_role_id;
  ModelInstanceId model_instance_id;
  ModelRevision model_revision;
  std::vector<std::string> compiled_collision_body_names;

  friend bool operator==(const ProgramCollisionBodyRoleBindingDTO&,
                         const ProgramCollisionBodyRoleBindingDTO&) = default;
};

struct ProgramControlBindingRefDTO final {
  ControlBindingId control_binding_id;
  ControlBindingRevision control_binding_revision;

  friend bool operator==(const ProgramControlBindingRefDTO&,
                         const ProgramControlBindingRefDTO&) = default;
};

struct ProgramCapabilityProviderBindingRefDTO final {
  CapabilityProviderBindingId capability_provider_binding_id;
  CapabilityProviderBindingRevision capability_provider_binding_revision;

  friend bool operator==(const ProgramCapabilityProviderBindingRefDTO&,
                         const ProgramCapabilityProviderBindingRefDTO&) =
      default;
};

struct ProgramCommandSchemaRefDTO final {
  CommandSchemaId command_schema_id;
  CommandSchemaRevision command_schema_revision;

  friend bool operator==(const ProgramCommandSchemaRefDTO&,
                         const ProgramCommandSchemaRefDTO&) = default;
};

struct ProgramCapabilityBindingDTO final {
  CapabilityId capability_id;
  CapabilityDefinitionRevision capability_definition_revision;
  std::optional<ProgramControlBindingRefDTO> control_binding;
  std::optional<ProgramCapabilityProviderBindingRefDTO>
      capability_provider_binding;
  std::optional<ProgramCommandSchemaRefDTO> command_schema;

  friend bool operator==(const ProgramCapabilityBindingDTO&,
                         const ProgramCapabilityBindingDTO&) = default;
};

struct ProgramTargetRoleBindingDTO final {
  TargetRoleId target_role_id;
  ComponentRoleId component_role_id;
  ControllableComponentId controllable_component_id;
  CommandTargetId command_target_id;
  std::vector<ProgramFrameRoleBindingDTO> frame_role_bindings;
  std::vector<ProgramResourceRoleBindingDTO> resource_role_bindings;
  std::vector<ProgramCollisionBodyRoleBindingDTO>
      collision_body_role_bindings;
  std::vector<ProgramCapabilityBindingDTO> capability_bindings;

  friend bool operator==(const ProgramTargetRoleBindingDTO&,
                         const ProgramTargetRoleBindingDTO&) = default;
};

struct ProgramBindingProvenanceDTO final {
  ContentDigest binding_compiler_contract_digest;
  ContentDigest binding_compiler_build_digest;
  ContentDigest role_assignment_digest;
  std::vector<RetainedSourceRecordDTO> source_records;
  std::vector<RetainedSourceRecordDTO> resolved_output_records;

  friend bool operator==(const ProgramBindingProvenanceDTO&,
                         const ProgramBindingProvenanceDTO&) = default;
};

/// Complete closed `manyforge.program_binding.v1` semantic value.
///
/// This aggregate is an untrusted value representation. The deployment
/// package's binding compiler and reload authority are the only producers of
/// a sealed, identity-verified ProgramBinding.
struct ProgramBindingDTO final {
  std::string api_version;
  ProgramBindingId program_binding_id;
  ProgramId program_id;
  ProgramRevision program_revision;
  RetainedBundleId retained_bundle_id;
  GraphRevision graph_revision;
  RetainedNodeEnvironmentDTO node_environment;
  std::vector<ProgramComponentRoleBindingDTO> component_role_bindings;
  std::vector<ProgramTargetRoleBindingDTO> target_role_bindings;
  ProgramBindingProvenanceDTO provenance;

  friend bool operator==(const ProgramBindingDTO&,
                         const ProgramBindingDTO&) = default;
};

}  // namespace manyforge::core
