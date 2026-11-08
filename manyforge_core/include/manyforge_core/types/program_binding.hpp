#pragma once

#include <manyforge_core/types/contact_policy.hpp>

#include <manyforge_core/types/program_contract.hpp>
#include <manyforge_core/types/io_signal.hpp>
#include <manyforge_core/types/retained_product.hpp>
#include <manyforge_core/types/tcp_entry.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::core {

inline constexpr std::string_view kProgramBindingApiVersion =
    "manyforge.program_binding.v3";
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

struct ProgramComponentBindingDTO final {
  ControllableComponentId component_id;
  ComponentTypeId component_type_id;
  ComponentTypeRevision component_type_revision;
  std::vector<ProgramModelRoleBindingDTO> model_bindings;

  friend bool operator==(const ProgramComponentBindingDTO&,
                         const ProgramComponentBindingDTO&) = default;
};

struct ProgramFrameBindingDTO final {
  ProgramFrameReferenceDTO reference;
  ModelInstanceId model_instance_id;
  ModelRevision model_revision;
  std::string compiled_frame_name;

  friend bool operator==(const ProgramFrameBindingDTO&,
                         const ProgramFrameBindingDTO&) = default;
};

/// One selectable TCP known to a moving robot. `entry` names its owner while
/// `frame` carries the exact retained frame and the owner's contact bodies.
struct ProgramRobotTcpBindingDTO final {
  TcpEntryReferenceDTO entry;
  ProgramFrameBindingDTO frame;
  std::vector<CollisionBodyRoleId> contact_body_role_ids{};
  bool end_effector{};
  bool component_default{};

  friend bool operator==(const ProgramRobotTcpBindingDTO&,
                         const ProgramRobotTcpBindingDTO&) = default;
};

struct ProgramResourceRoleBindingDTO final {
  ResourceRoleId resource_role_id;
  std::vector<ResourceId> resource_ids;
  std::vector<std::string> compiled_resource_names;

  friend bool operator==(const ProgramResourceRoleBindingDTO&,
                         const ProgramResourceRoleBindingDTO&) = default;
};

struct ProgramCollisionBodyBindingDTO final {
  ProgramCollisionBodyReferenceDTO reference;
  ModelInstanceId model_instance_id;
  ModelRevision model_revision;
  std::vector<std::string> compiled_collision_body_names;

  friend bool operator==(const ProgramCollisionBodyBindingDTO&,
                         const ProgramCollisionBodyBindingDTO&) = default;
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

struct ProgramSignalBindingDTO final {
  ProgramSignalReferenceDTO reference;
  ProgramTargetReferenceDTO target;
  RuntimeUnitId runtime_unit_id;
  IoSignalValueType value_type;
  IoSignalDirection direction;
  IoTransitionCapability transition_capability;
  ContentDigest signal_descriptor_digest;

  friend bool operator==(const ProgramSignalBindingDTO&,
                         const ProgramSignalBindingDTO&) = default;
};

/// Compiler-owned references to the ordinary I/O roles of a pneumatic tool.
/// The output effects remain ordinary I/O operations; these rows do not prove
/// that a valve was delivered or that a grasp occurred.
struct ProgramPneumaticOutputBindingDTO final {
  ProgramSignalReferenceDTO signal;
  bool open_value{};
  bool close_value{};
  friend bool operator==(const ProgramPneumaticOutputBindingDTO&,
                         const ProgramPneumaticOutputBindingDTO&) = default;
};

struct ProgramPneumaticInputBindingDTO final {
  std::string input;
  ProgramSignalReferenceDTO signal;
  friend bool operator==(const ProgramPneumaticInputBindingDTO&,
                         const ProgramPneumaticInputBindingDTO&) = default;
};

struct ProgramPneumaticIoBindingDTO final {
  std::vector<ProgramPneumaticOutputBindingDTO> outputs;
  std::vector<ProgramPneumaticInputBindingDTO> inputs;
  friend bool operator==(const ProgramPneumaticIoBindingDTO&,
                         const ProgramPneumaticIoBindingDTO&) = default;
};

struct ProgramTargetFrameAssociationDTO final {
  ProgramFrameReferenceDTO frame;
  std::vector<CollisionBodyRoleId> contact_body_role_ids;

  friend bool operator==(const ProgramTargetFrameAssociationDTO&,
                         const ProgramTargetFrameAssociationDTO&) = default;
};

struct ProgramTargetBindingDTO final {
  ProgramTargetReferenceDTO target;
  std::vector<ProgramFrameReferenceDTO> frame_references;
  std::vector<ProgramCollisionBodyReferenceDTO> collision_body_references;
  std::vector<ProgramSignalReferenceDTO> signal_references;
  std::vector<ProgramResourceRoleBindingDTO> resource_role_bindings;
  std::vector<ProgramCapabilityBindingDTO> capability_bindings;
  // Omitted for all targets without an authored pneumatic I/O mapping.
  std::optional<ProgramPneumaticIoBindingDTO> pneumatic_io{};
  // Present for a trajectory-execution target, including an empty list;
  // omitted for other targets. Rows cover this robot's own TCPs and every
  // tool component's TCPs in the Deployment.
  std::optional<std::vector<ProgramRobotTcpBindingDTO>> robot_tcps{};
  // The target component's explicit or declared default TCP, if any.
  std::optional<TcpEntryReferenceDTO> default_tcp{};
  // Exact target/frame associations; geometry stays in root frame bindings.
  std::vector<ProgramTargetFrameAssociationDTO> frame_associations{};

  friend bool operator==(const ProgramTargetBindingDTO&,
                         const ProgramTargetBindingDTO&) = default;
};

/// One direct body selector resolved through the exact retained model map. `collision_body_ids` use the immutable component/model/
/// role/index identity consumed by the retained collision-world projector.
struct ProgramContactGraspBodyBindingDTO final {
  ProgramCollisionBodyReferenceDTO body;
  std::optional<ModelRoleId> model_role_id;
  ModelInstanceId model_instance_id;
  ModelRevision model_revision;
  std::vector<std::string> collision_body_ids;

  friend bool operator==(const ProgramContactGraspBodyBindingDTO&,
                         const ProgramContactGraspBodyBindingDTO&) = default;
};

/// Closed Program-Binding form of one geometry-only contact-grasp template.
/// The runtime object remains portable until an operation binds it to one
/// current logical-scene resource.
struct ProgramContactGraspBindingDTO final {
  ContactPolicyId contact_policy_id;
  ContactPolicyRevision contact_policy_revision;
  RuntimeObjectId runtime_object_id;
  std::vector<ProgramContactGraspBodyBindingDTO> allowed_body_bindings;
  ContactZoneEnvelopeDTO envelope{ContactGraspEnvelopeDTO{}};

  /// Omitted from canonical bytes when false. Endpoint bodies are selected
  /// only at operation admission, never mixed with explicit body bindings.
  bool bodies_from_motion_endpoint{};

  friend bool operator==(const ProgramContactGraspBindingDTO&,
                         const ProgramContactGraspBindingDTO&) = default;
};

struct ProgramBindingProvenanceDTO final {
  ContentDigest binding_compiler_contract_digest;
  std::vector<RetainedSourceRecordDTO> source_records;
  std::vector<RetainedSourceRecordDTO> resolved_output_records;

  friend bool operator==(const ProgramBindingProvenanceDTO&,
                         const ProgramBindingProvenanceDTO&) = default;
};

/// Complete closed `manyforge.program_binding.v3` semantic value.
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
  std::vector<ProgramComponentBindingDTO> component_bindings;
  std::vector<ProgramTargetBindingDTO> target_bindings;
  std::vector<ProgramFrameBindingDTO> frame_bindings;
  std::vector<ProgramSignalBindingDTO> signal_bindings;
  std::vector<ProgramCollisionBodyBindingDTO> collision_body_bindings;
  ProgramBindingProvenanceDTO provenance;
  std::vector<ProgramContactGraspBindingDTO> contact_grasp_bindings{};
  std::vector<ProgramNodePolicyDTO> node_policies{};

  friend bool operator==(const ProgramBindingDTO&,
                         const ProgramBindingDTO&) = default;
};

}  // namespace manyforge::core
