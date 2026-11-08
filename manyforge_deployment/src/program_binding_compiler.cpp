#include "manyforge_deployment/program_binding_compiler.hpp"

#include <algorithm>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "build_manifest_internal.hpp"
#include "manyforge_core/types/capability_definition.hpp"
#include "manyforge_core/types/collision_world_snapshot.hpp"
#include "manyforge_core/types/command_schema.hpp"
#include "manyforge_deployment/artifact_codec.hpp"
#include "manyforge_deployment/capability_definition_codec.hpp"
#include "manyforge_deployment/cell_kinematic_index.hpp"
#include "manyforge_deployment/command_schema_codec.hpp"
#include "manyforge_deployment/component_type_codec.hpp"
#include "manyforge_deployment/content_identity.hpp"
#include "program_binding_internal.hpp"
#include "program_reference_probe_internal.hpp"
#include "program_io_condition_compiler_internal.hpp"
#include <manyforge_core/io_signal_descriptor.hpp>
#include <manyforge_deployment/io_signal_catalog.hpp>
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>
#include "ros2_control_execution_runtime_unit_internal.hpp"

namespace manyforge::deployment {
using namespace reference_probe;
namespace {

constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";

struct DirectCapabilityBindings final {
  std::vector<core::ProgramCapabilityBindingDTO> capabilities;
  std::set<core::ResourceRoleId> resource_roles;
};

[[nodiscard]] DirectCapabilityBindings resolve_direct_capabilities(
    const core::ProgramTargetRequirementDTO& requirement,
    const core::ComponentTypeCommandTargetDTO& target_type,
    const core::ControllableComponentDTO& component,
    const core::RetainedBundleDTO& bundle,
    const QualifiedCatalogClosure& catalog,
    const VerifiedRetainedModelMap& model_map,
    std::span<const RetainedIoSignalConfiguration> signal_catalog) {
  DirectCapabilityBindings result;
  for (const auto& requested : requirement.required_capabilities) {
    const bool target_offers = std::any_of(target_type.capabilities.begin(), target_type.capabilities.end(),
        [&](const auto& value) {
          return value.capability_id == requested.capability_id &&
                 value.capability_definition_revision == requested.capability_definition_revision;
        });
    const auto input_use = std::find_if(requirement.input_signal_uses.begin(),
        requirement.input_signal_uses.end(), [&](const auto& value) {
          return value.capability_id == requested.capability_id;
        });
    if (!target_offers && input_use != requirement.input_signal_uses.end() &&
        input_use->all_uses_are_signal_reads && !input_use->signals.empty() &&
        std::all_of(input_use->signals.begin(), input_use->signals.end(), [&](const auto& reference) {
          const auto matches = FindProgramSignalMatches(signal_catalog, reference);
          return matches.size() == 1U && reference.component_id == requirement.target.component_id &&
                 matches.front().retained->command_target_id == requirement.target.command_target_id &&
                 matches.front().descriptor.direction == core::IoSignalDirection::kInput &&
                 matches.front().retained->provider.profile == kSystemCarriedObjectProfile;
        }))
      continue;
    auto capability = resolve_capability(requested, target_type, component,
                                         bundle.cell_graph, bundle, catalog, model_map);
    if (!capability) continue;
    auto roles = capability_resource_roles(*capability, requirement.target,
                                           bundle.cell_graph);
    result.resource_roles.insert(roles.begin(), roles.end());
    if (capability->command_schema) {
      const auto& reference = *capability->command_schema;
      const auto* schema = find_exactly_one<QualifiedCommandSchema>(
          catalog.command_schemas, [&](const auto& candidate) {
            return candidate.value().command_schema_id == reference.command_schema_id &&
                   candidate.value().command_schema_revision == reference.command_schema_revision;
          });
      if (!schema) fail("resolved capability has no unique retained command schema");
      auto schema_roles = command_schema_resource_roles(*schema, target_type);
      result.resource_roles.insert(schema_roles.begin(), schema_roles.end());
    }
    result.capabilities.push_back(std::move(*capability));
  }
  return result;
}

[[nodiscard]] auto source_record_key(
    const core::RetainedSourceRecordDTO& value) {
  return std::visit(
      [](const auto& record) {
        using Record = std::remove_cvref_t<decltype(record)>;
        return std::tuple(std::string(core::to_string(Record::kRecordKind)),
                          record.record_id.value(), record.revision.value());
      },
      value);
}

void add_resolved_source_record(
    const core::RetainedBundleDTO& bundle, core::CatalogRecordKind kind,
    std::string_view id, std::string_view revision,
    std::vector<core::RetainedSourceRecordDTO>& output) {
  const core::RetainedSourceRecordDTO* match = nullptr;
  for (const auto& record : bundle.provenance.source_records) {
    const bool exact = std::visit(
        [&](const auto& typed) {
          using Record = std::remove_cvref_t<decltype(typed)>;
          return Record::kRecordKind == kind &&
                 typed.record_id.value() == id &&
                 typed.revision.value() == revision;
        },
        record);
    if (!exact) {
      continue;
    }
    if (match != nullptr) {
      fail("resolved provenance contains a duplicate exact source record");
    }
    match = &record;
  }
  if (match == nullptr) {
    fail("resolved binding identity is absent from bundle provenance");
  }
  if (std::none_of(output.begin(), output.end(), [&](const auto& existing) {
        return source_record_key(existing) == source_record_key(*match);
      })) {
    output.push_back(*match);
  }
}

void add_component_provenance(
    const core::RetainedBundleDTO& bundle,
    const core::ProgramComponentBindingDTO& component,
    std::vector<core::RetainedSourceRecordDTO>& output) {
  add_resolved_source_record(
      bundle, core::CatalogRecordKind::kComponentType,
      component.component_type_id.value(),
      component.component_type_revision.value(), output);
  for (const auto& model_binding : component.model_bindings) {
    const auto& model = require_model(bundle.cell_graph,
                                      model_binding.model_instance_id);
    add_resolved_source_record(bundle, core::CatalogRecordKind::kModelAsset,
                               model.model_asset_id.value(),
                               model.model_revision.value(), output);
  }
}

void add_capability_provenance(
    const core::RetainedBundleDTO& bundle,
    const core::ProgramCapabilityBindingDTO& capability,
    std::vector<core::RetainedSourceRecordDTO>& output) {
  add_resolved_source_record(
      bundle, core::CatalogRecordKind::kCapabilityDefinition,
      capability.capability_id.value(),
      capability.capability_definition_revision.value(), output);
  if (capability.command_schema.has_value()) {
    add_resolved_source_record(
        bundle, core::CatalogRecordKind::kCommandSchema,
        capability.command_schema->command_schema_id.value(),
        capability.command_schema->command_schema_revision.value(), output);
  }
  if (capability.control_binding.has_value()) {
    const auto* binding = find_control_binding(
        bundle.cell_graph, capability.control_binding->control_binding_id);
    if (binding == nullptr) {
      fail("resolved control binding disappeared while building provenance");
    }
    add_resolved_source_record(
        bundle, core::CatalogRecordKind::kControlInterfaceContract,
        binding->control_interface_contract_id.value(),
        binding->control_interface_revision.value(), output);
    add_resolved_source_record(
        bundle, core::CatalogRecordKind::kControllerTopology,
        binding->controller_topology_id.value(),
        binding->topology_revision.value(), output);
    add_resolved_source_record(
        bundle, core::CatalogRecordKind::kProviderManifest,
        binding->provider_manifest_id.value(),
        binding->provider_manifest_revision.value(), output);
    add_resolved_source_record(
        bundle, core::CatalogRecordKind::kStopEvidencePolicy,
        binding->stop_evidence_policy_id.value(),
        binding->stop_evidence_policy_revision.value(), output);
  }
  if (capability.capability_provider_binding.has_value()) {
    const auto* provider =
        find_exactly_one<core::CapabilityProviderBindingDTO>(
            bundle.capability_provider_bindings, [&](const auto& value) {
              return value.capability_provider_binding_id ==
                     capability.capability_provider_binding
                         ->capability_provider_binding_id;
            });
    if (provider == nullptr) {
      fail("resolved provider binding disappeared while building provenance");
    }
    add_resolved_source_record(
        bundle, core::CatalogRecordKind::kProviderManifest,
        provider->provider_manifest_id.value(),
        provider->provider_manifest_revision.value(), output);
  }
}

[[nodiscard]] std::vector<core::ProgramContactGraspBindingDTO>
resolve_contact_grasp_bindings(
    const core::ProgramBindingContractDTO& contract,
    const std::vector<core::ProgramComponentBindingDTO>& components,
    const std::vector<core::ProgramCollisionBodyBindingDTO>& collision_bodies) {
  std::vector<core::ProgramContactGraspBindingDTO> resolved;
  resolved.reserve(contract.contact_grasp_templates.size());
  for (const auto& policy : contract.contact_grasp_templates) {
    std::vector<core::ProgramContactGraspBodyBindingDTO> bodies;
    bodies.reserve(policy.allowed_body_selectors.size());
    for (const auto& selector : policy.allowed_body_selectors) {
      const auto* component = find_exactly_one<core::ProgramComponentBindingDTO>(
          components, [&](const auto& row) {
            return row.component_id == selector.body.component_id;
          });
      const auto* body = find_exactly_one<core::ProgramCollisionBodyBindingDTO>(
          collision_bodies, [&](const auto& row) {
            return row.reference == selector.body;
          });
      if (!component || !body)
        fail("contact-grasp selector has no unique component/body binding");
      const bool model_owned = std::any_of(
          component->model_bindings.begin(), component->model_bindings.end(),
          [&](const auto& row) {
            return row.model_instance_id == body->model_instance_id &&
                   row.model_revision == body->model_revision;
          });
      if (!model_owned)
        fail("contact-grasp body has no exact component model binding");
      std::vector<core::CollisionBodyId> body_ids;
      body_ids.reserve(body->compiled_collision_body_names.size());
      for (std::size_t index = 0; index < body->compiled_collision_body_names.size(); ++index)
        body_ids.push_back(core::make_component_collision_body_id(
            selector.body.component_id, body->model_instance_id,
            selector.body.collision_body_id, index));
      bodies.push_back(core::ProgramContactGraspBodyBindingDTO{
          selector.body, std::nullopt, body->model_instance_id,
          body->model_revision, std::move(body_ids)});
    }
    resolved.push_back(core::ProgramContactGraspBindingDTO{
        policy.contact_policy_id, policy.contact_policy_revision,
        policy.runtime_object_id, std::move(bodies), policy.envelope,
        policy.bodies_from_motion_endpoint});
  }
  return resolved;
}

}  // namespace

namespace {

using DirectControlResourceRequirements =
    std::map<core::ProgramTargetReferenceDTO, std::set<core::ResourceRoleId>>;

struct DirectReferenceBindings final {
  std::vector<core::ProgramFrameBindingDTO> frames;
  std::vector<core::ProgramSignalBindingDTO> signals;
  std::vector<core::ProgramCollisionBodyBindingDTO> bodies;
};


struct DirectPneumaticBindings final {
  core::ProgramPneumaticIoBindingDTO route;
  std::vector<core::ProgramSignalBindingDTO> signals;
};

using RetainedPneumaticIo =
    ros2_control_execution_runtime_contract::Configuration::PneumaticIo;

[[nodiscard]] std::map<core::ProgramTargetReferenceDTO, RetainedPneumaticIo>
retained_pneumatic_configurations(const core::CellGraphDTO& graph) {
  std::map<core::ProgramTargetReferenceDTO, RetainedPneumaticIo> result;
  for (const auto& unit : graph.runtime_units) {
    if (unit.runtime_profile_id.value() != ros2_control_execution_runtime_contract::kRuntimeProfileId)
      continue;
    const auto rows = unit.configuration.find("gripper_target_configurations");
    if (rows == unit.configuration.end()) continue;
    const auto* array = std::get_if<core::JsonValueDTO::Array>(&rows->second.value);
    if (!array) fail("retained gripper configuration is not an array");
    for (const auto& row : detail::decode_ros2_control_execution_gripper_target_configurations(*array)) {
      if (row.pneumatic_io &&
          !result.emplace(core::ProgramTargetReferenceDTO{row.controllable_component_id,
                                                          row.command_target_id},
                          *row.pneumatic_io).second)
        fail("retained pneumatic target has multiple I/O mappings");
    }
  }
  return result;
}

[[nodiscard]] DirectPneumaticBindings resolve_direct_pneumatic_bindings(
    const ros2_control_execution_runtime_contract::Configuration::PneumaticIo& configuration,
    std::span<const RetainedIoSignalConfiguration> catalog) {
  DirectPneumaticBindings result;
  const auto bind = [&](const core::IoSignalId& id, core::IoSignalDirection direction) {
    core::ProgramSignalReferenceDTO reference{id.controllable_component_id, id.name};
    const auto matches = FindProgramSignalMatches(catalog, reference);
    if (matches.size() != 1U || matches.front().descriptor.direction != direction)
      fail("pneumatic I/O signal is missing, ambiguous or has the wrong direction");
    auto binding = resolve_program_signal_binding(reference, catalog);
    const auto existing = std::find_if(result.signals.begin(), result.signals.end(),
                                       [&](const auto& row) { return row.reference == reference; });
    if (existing == result.signals.end()) result.signals.push_back(std::move(binding));
    else if (*existing != binding) fail("pneumatic I/O signal has conflicting bindings");
    return reference;
  };
  for (const auto& output : configuration.outputs)
    result.route.outputs.push_back({bind(output.signal, core::IoSignalDirection::kOutput),
                                    output.open_value, output.close_value});
  for (const auto& [name, input] : configuration.inputs)
    result.route.inputs.push_back({name, bind(input, core::IoSignalDirection::kInput)});
  std::sort(result.signals.begin(), result.signals.end(), [](const auto& left, const auto& right) {
    return left.reference < right.reference;
  });
  return result;
}

[[nodiscard]] std::vector<core::ProgramCollisionBodyBindingDTO>
resolve_target_contact_bodies(
    std::span<const core::ProgramTargetFrameAssociationDTO> associations,
    std::span<const core::ProgramFrameBindingDTO> frames,
    const VerifiedRetainedModelMap& model_map) {
  std::vector<core::ProgramCollisionBodyBindingDTO> result;
  for (const auto& association : associations) {
    const auto* frame = find_exactly_one<core::ProgramFrameBindingDTO>(
        frames, [&](const auto& value) { return value.reference == association.frame; });
    if (!frame) fail("target contact frame has no unique resolved geometry");
    for (const auto& role : association.contact_body_role_ids) {
      const core::ProgramCollisionBodyReferenceDTO reference{association.frame.component_id, role};
      auto matches = find_program_collision_body_matches(reference, model_map);
      if (matches.size() != 1U || matches.front().model_instance_id != frame->model_instance_id ||
          matches.front().model_revision != frame->model_revision)
        fail("target contact body has no unique matching retained frame model");
      auto found = std::find_if(result.begin(), result.end(), [&](const auto& value) {
        return value.reference == reference;
      });
      if (found == result.end()) result.push_back(std::move(matches.front()));
      else if (*found != matches.front()) fail("target contact body has conflicting resolutions");
    }
  }
  std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
    return left.reference < right.reference;
  });
  return result;
}

// DX3822: retain unambiguous component-owned aliases and model frames for
// every component named by the Program, including dynamic stored-pose use.
// DX3823 leaves ambiguous unused names out of sealed rows; an authored
// ambiguous reference is still refused by the shared validator.
[[nodiscard]] std::vector<core::ProgramFrameBindingDTO> component_frame_closure(
    const std::set<core::ControllableComponentId>& components,
    const VerifiedRetainedModelMap& model_map) {
  std::vector<core::ProgramFrameBindingDTO> result;
  for (const auto& name : program_component_frame_names(components, model_map)) {
    auto matches = find_program_frame_matches(name, model_map);
    if (matches.size() == 1U) result.push_back(std::move(matches.front()));
    // An ambiguous unused name is not a resolved Binding row. Runtime
    // checks the retained facts rather than treating omission as absence.
  }
  return result;
}

struct DirectComponentBindings final {
  std::vector<core::ProgramComponentBindingDTO> components;
  std::map<core::ControllableComponentId, QualifiedComponentType> types;
};

[[nodiscard]] DirectComponentBindings resolve_direct_components(
    const std::set<core::ControllableComponentId>& references,
    const core::RetainedBundleDTO& bundle,
    const QualifiedCatalogClosure& catalog,
    const ICatalogArtifactSource& catalog_artifact_store) {
  DirectComponentBindings result;
  for (const auto& id : references) {
    const auto& component = require_component(bundle.cell_graph, id);
    auto type = load_component_type(bundle, component, catalog.capabilities,
                                    catalog_artifact_store);
    result.components.push_back(resolve_component_binding(id, bundle.cell_graph, type));
    result.types.emplace(id, std::move(type));
  }
  return result;
}

[[nodiscard]] DirectReferenceBindings resolve_direct_reference_bindings(
    const core::ProgramBindingContractDTO& contract,
    const VerifiedRetainedModelMap& model_map,
    std::span<const RetainedIoSignalConfiguration> signal_catalog) {
  DirectReferenceBindings result;
  for (const auto& reference : contract.frame_requirements) {
    auto matches = find_program_frame_matches(reference, model_map);
    if (matches.size() != 1U)
      fail("Program frame reference has no unique retained resolution");
    result.frames.push_back(std::move(matches.front()));
  }
  for (const auto& reference : contract.signal_requirements)
    result.signals.push_back(resolve_program_signal_binding(reference, signal_catalog));
  for (const auto& reference : contract.collision_body_requirements) {
    auto matches = find_program_collision_body_matches(reference, model_map);
    if (matches.size() != 1U)
      fail("Program collision-body reference has no unique retained resolution");
    result.bodies.push_back(std::move(matches.front()));
  }
  return result;
}


struct DirectTargetBindings final {
  core::ProgramTargetBindingDTO target;
  std::vector<core::ProgramCollisionBodyBindingDTO> contact_bodies;
};

[[nodiscard]] std::shared_ptr<const ProgramBinding> seal_direct_program_binding(
    const core::ProgramBindingContractDTO& contract,
    const RetainedBundle& retained_bundle,
    const VerifiedRetainedModelMap& model_map,
    const ICatalogArtifactSource& catalog_artifact_store,
    std::vector<core::ProgramComponentBindingDTO> components,
    std::vector<core::ProgramTargetBindingDTO> targets,
    DirectReferenceBindings references) {
  const auto& bundle = retained_bundle.value();
  std::vector<core::RetainedSourceRecordDTO> resolved_records;
  for (const auto& component : components)
    add_component_provenance(bundle, component, resolved_records);
  for (const auto& target : targets)
    for (const auto& capability : target.capability_bindings)
      add_capability_provenance(bundle, capability, resolved_records);
  auto contact = resolve_contact_grasp_bindings(contract, components, references.bodies);
  const auto current = detail::BuildQualifiedRetentionAuthority::qualify();
  core::ProgramBindingDTO value{
      std::string(core::kProgramBindingApiVersion), core::ProgramBindingId(std::string(kZeroDigest)),
      contract.program_id, contract.program_revision, retained_bundle.retained_bundle_id(),
      bundle.cell_graph.graph_revision, bundle.catalogs.node_environment,
      std::move(components), std::move(targets), std::move(references.frames),
      std::move(references.signals), std::move(references.bodies),
      {current.compiler_contract_digest(), bundle.provenance.source_records,
       std::move(resolved_records)},
      std::move(contact), contract.node_policies};
  auto sealed = detail::ProgramBindingConstructionAuthority::seal(std::move(value));
  return reload_verified_program_binding(sealed->canonical_bytes(), retained_bundle, model_map,
                                         catalog_artifact_store);
}

[[nodiscard]] DirectTargetBindings resolve_direct_target_bindings(
    const core::ProgramTargetRequirementDTO& requirement,
    const core::ControllableComponentDTO& component,
    const core::ComponentTypeCommandTargetDTO& target_type,
    const core::RetainedBundleDTO& bundle,
    const QualifiedCatalogClosure& catalog,
    const VerifiedRetainedModelMap& model_map,
    std::span<const core::ProgramFrameBindingDTO> frames,
    std::span<const core::ProgramCollisionBodyBindingDTO> bodies,
    std::span<const RetainedIoSignalConfiguration> signal_catalog,
    const std::set<core::ResourceRoleId>* intrinsic_resources) {
  const auto* target_model = model_map.find_target(requirement.target.component_id,
                                                    requirement.target.command_target_id);
  if (!target_model)
    unavailable(ProgramBindingUnresolvedReason::kTargetIncompatible,
                "target has no exact retained model resolution");
  auto capability = resolve_direct_capabilities(requirement, target_type, component,
                                                bundle, catalog, model_map, signal_catalog);
  const auto capability_roles = capability.resource_roles;
  if (intrinsic_resources)
    capability.resource_roles.insert(intrinsic_resources->begin(), intrinsic_resources->end());
  const auto signal_roles = signal_resource_roles(requirement.target,
                                                   requirement.signal_references,
                                                   signal_catalog);
  std::vector<core::ProgramResourceRoleBindingDTO> resources;
  for (const auto& role : capability.resource_roles) {
    if (signal_roles.contains(role)) {
      if (capability_roles.contains(role))
        fail("target resource role is required as both a coordinate and a signal");
      continue;
    }
    resources.push_back(resolve_resource_role(model_map, target_type,
                                               requirement.target.component_id,
                                               requirement.target.command_target_id,
                                               role).binding);
  }
  auto frame_associations = resolve_target_frame_associations(requirement.target, requirement.frame_references, target_type,
                                                               *target_model, model_map, frames);
  auto contact_bodies = resolve_target_contact_bodies(frame_associations, frames, model_map);
  auto body_references = requirement.collision_body_references;
  for (const auto& reference : body_references) {
    const auto* qualified = find_exactly_one<core::ProgramCollisionBodyBindingDTO>(
        bodies, [&](const auto& body) { return body.reference == reference; });
    if (!qualified)
      fail("target collision body has no unique qualified Program binding");
    const bool target_provides = std::any_of(
        target_model->collision_bodies.begin(), target_model->collision_bodies.end(),
        [&](const auto& candidate) {
          return candidate.model_instance_id == qualified->model_instance_id &&
                 !qualified->compiled_collision_body_names.empty() &&
                 std::all_of(qualified->compiled_collision_body_names.begin(),
                             qualified->compiled_collision_body_names.end(),
                             [&](const auto &name) {
                               return std::find(candidate.compiled_link_names.begin(),
                                                candidate.compiled_link_names.end(), name) !=
                                      candidate.compiled_link_names.end();
                             });
        });
    if (!target_provides ||
        !model_map.kinematic_index().carries(requirement.target.component_id,
                                             reference.component_id))
      unavailable(ProgramBindingUnresolvedReason::kTargetIncompatible,
                  "target collision body is unavailable or not mounted");
  }
  for (const auto& body : contact_bodies) body_references.push_back(body.reference);
  std::sort(body_references.begin(), body_references.end());
  body_references.erase(std::unique(body_references.begin(), body_references.end()),
                        body_references.end());
  return {core::ProgramTargetBindingDTO{
              requirement.target, requirement.frame_references, std::move(body_references),
              requirement.signal_references, std::move(resources), std::move(capability.capabilities),
              std::nullopt, std::nullopt, std::nullopt, std::move(frame_associations)},
          std::move(contact_bodies)};
}

struct DirectTargetCollector final {
  std::vector<ProgramBindingUnresolvedTargetDTO> rows;
  ProgramResolvedReferencesDTO resolved;
  // Derived pneumatic signal and default-TCP targets are dependencies of
  // the authored target. Attribution never creates a new node effect.
  std::map<core::ProgramTargetReferenceDTO, std::set<core::ProgramTargetReferenceDTO>>
      derived_dependents;
};

[[nodiscard]] std::vector<core::ProgramTargetBindingDTO>
resolve_direct_targets(
    const core::ProgramBindingContractDTO& contract,
    const core::RetainedBundleDTO& bundle,
    const QualifiedCatalogClosure& catalog,
    const VerifiedRetainedModelMap& model_map,
    const DirectComponentBindings& components,
    DirectReferenceBindings& references,
    std::span<const RetainedIoSignalConfiguration> signal_catalog,
    const DirectControlResourceRequirements* intrinsic_resources,
    DirectTargetCollector* collector) {
  std::vector<core::ProgramTargetBindingDTO> targets;
  for (const auto& requirement : contract.target_requirements) {
    try {
      const auto& component = require_component(bundle.cell_graph, requirement.target.component_id);
      const auto type = components.types.find(requirement.target.component_id);
      if (type == components.types.end()) fail("target component has no qualified type");
      const auto& target_type = require_target_type(type->second, requirement.target.command_target_id);
      const std::set<core::ResourceRoleId>* intrinsic = nullptr;
      if (intrinsic_resources) {
        const auto found = intrinsic_resources->find(requirement.target);
        if (found != intrinsic_resources->end()) intrinsic = &found->second;
      }
      auto resolved = resolve_direct_target_bindings(requirement, component, target_type,
                                                      bundle, catalog, model_map,
                                                      references.frames, references.bodies,
                                                      signal_catalog, intrinsic);
      for (auto& body : resolved.contact_bodies) {
        const auto existing = std::find_if(references.bodies.begin(), references.bodies.end(),
                                           [&](const auto& row) { return row.reference == body.reference; });
        if (existing == references.bodies.end()) references.bodies.push_back(std::move(body));
        else if (*existing != body) fail("derived contact body conflicts with Program body");
      }
      targets.push_back(std::move(resolved.target));
    } catch (const TargetUnavailable& error) {
      if (!collector) throw;
      std::string detail(error.what());
      detail.resize(std::min(detail.size(), kMaximumProgramBindingUnresolvedDetailBytes));
      collector->rows.push_back({requirement.target, error.reason(), std::move(detail)});
    }
  }
  std::sort(references.bodies.begin(), references.bodies.end(),
            [](const auto& left, const auto& right) { return left.reference < right.reference; });
  if (collector) collector->resolved.frame_bindings = references.frames;
  return targets;
}

struct DirectTcpBindings final {
  std::vector<RobotTcp> tcps;
  std::optional<core::TcpEntryReferenceDTO> default_tcp;
  std::vector<core::ProgramFrameBindingDTO> frames;
  std::vector<core::ProgramCollisionBodyBindingDTO> contact_bodies;
};

[[nodiscard]] std::optional<core::TcpEntryReferenceDTO> resolve_direct_default_tcp(
    const core::ControllableComponentId& component,
    const VerifiedRetainedModelMap& model_map) {
  const auto* value = model_map.kinematic_index().default_tcp_of(component);
  if (!value) return std::nullopt;
  const auto* owner_target = model_map.find_target(component, value->command_target_id);
  const auto* role = owner_target ? find_target_frame_role(*owner_target, value->frame_role_id) : nullptr;
  const auto* frame = role ? std::get_if<CompositionComponentModelFrame>(&role->frame) : nullptr;
  if (!frame || !model_map.kinematic_index().carries(component, frame->controllable_component_id))
    fail("default TCP has no exact mounted retained frame resolution");
  const core::ProgramFrameReferenceDTO reference{component, value->frame_role_id};
  const auto matches = find_program_frame_matches(reference, model_map);
  if (matches.size() != 1U || matches.front().model_instance_id != frame->model_instance_id ||
      matches.front().model_revision != frame->model_revision ||
      matches.front().compiled_frame_name != frame->compiled_frame_name)
    fail("default TCP alias has no unique qualified retained frame");
  return core::TcpEntryReferenceDTO{{component, value->command_target_id}, reference};
}

[[nodiscard]] DirectTcpBindings resolve_direct_tcp_bindings(
    const core::ProgramTargetReferenceDTO& target,
    const core::RetainedBundleDTO& bundle,
    const VerifiedRetainedModelMap& model_map,
    std::span<const QualifiedCapabilityDefinition> capabilities,
    const ICatalogArtifactSource& catalog_artifact_store) {
  DirectTcpBindings result;
  result.tcps = compile_robot_tcps(bundle, model_map, target.component_id,
                                   target.command_target_id, capabilities,
                                   catalog_artifact_store);
  result.default_tcp = resolve_direct_default_tcp(target.component_id, model_map);
  for (const auto& tcp : result.tcps) {
    const auto frame = std::find_if(result.frames.begin(), result.frames.end(),
                                    [&](const auto& row) { return row.reference == tcp.frame.reference; });
    if (frame == result.frames.end()) result.frames.push_back(tcp.frame);
    else if (*frame != tcp.frame) fail("robot TCP frames have conflicting retained geometry");
    const std::vector<core::ProgramTargetFrameAssociationDTO> association{
        {tcp.frame.reference, tcp.contact_body_role_ids}};
    const std::vector<core::ProgramFrameBindingDTO> geometry{tcp.frame};
    auto bodies = resolve_target_contact_bodies(association, geometry, model_map);
    for (auto& body : bodies) {
      const auto existing = std::find_if(result.contact_bodies.begin(), result.contact_bodies.end(),
                                         [&](const auto& row) { return row.reference == body.reference; });
      if (existing == result.contact_bodies.end()) result.contact_bodies.push_back(std::move(body));
      else if (*existing != body) fail("robot TCP contact bodies have conflicting retained geometry");
    }
  }
  std::sort(result.frames.begin(), result.frames.end(),
            [](const auto& left, const auto& right) { return left.reference < right.reference; });
  std::sort(result.contact_bodies.begin(), result.contact_bodies.end(),
            [](const auto& left, const auto& right) { return left.reference < right.reference; });
  return result;
}

[[nodiscard]] std::shared_ptr<const ProgramBinding>
compile_program_binding_contract(
    const core::ProgramBindingContractDTO& program_contract,
    const RetainedBundle& retained_bundle,
    const VerifiedRetainedModelMap& retained_model_map,
    const ICatalogArtifactSource& catalog_artifact_store,
    const DirectControlResourceRequirements* intrinsic_resources = nullptr,
    DirectTargetCollector* collector = nullptr) {
  try {
    if (!verify_retained_bundle_identity(retained_bundle) ||
        retained_model_map.query().retained_bundle_id != retained_bundle.retained_bundle_id())
      fail("binding compiler inputs do not share one verified retained bundle");
    const auto& bundle = retained_bundle.value();
    if (program_contract.node_environment != bundle.catalogs.node_environment)
      fail("Program contract selects another retained node environment");
    const auto catalog = load_qualified_catalog_closure(bundle, catalog_artifact_store);
    const auto io = ResolveCompiledIoSignalCatalog(bundle.cell_graph);
    if (!io.ok()) fail(io.error().message);
    const auto pneumatic = retained_pneumatic_configurations(bundle.cell_graph);
    auto contract = program_contract;
    std::map<core::ProgramTargetReferenceDTO, core::ProgramTargetRequirementDTO> requirements;
    for (const auto& requirement : contract.target_requirements)
      if (!requirements.emplace(requirement.target, requirement).second)
        fail("Program target requirements repeat an exact target");
    std::map<core::ProgramTargetReferenceDTO, core::ProgramPneumaticIoBindingDTO> routes;
    for (const auto& requirement : program_contract.target_requirements) {
      const bool gripper = std::any_of(requirement.required_capabilities.begin(),
          requirement.required_capabilities.end(), [](const auto& capability) {
            return capability.capability_id.value() == "tool.gripper.command";
          });
      const auto config = pneumatic.find(requirement.target);
      if (!gripper || config == pneumatic.end()) continue;
      auto derived = resolve_direct_pneumatic_bindings(config->second, io.value());
      routes.emplace(requirement.target, std::move(derived.route));
      for (const auto& signal : derived.signals) {
        if (collector)
          collector->derived_dependents[signal.target].insert(requirement.target);
        contract.signal_requirements.push_back(signal.reference);
        auto [it, inserted] = requirements.try_emplace(signal.target,
            core::ProgramTargetRequirementDTO{signal.target, {}, {}, {}, {}});
        (void)inserted;
        it->second.signal_references.push_back(signal.reference);
        const auto add_capability = [&](std::string_view id) {
          const auto* descriptor = core::find_builtin_capability_definition(id);
          if (!descriptor) fail("pneumatic I/O capability definition is unavailable");
          const core::ProgramCapabilityRequirementDTO requested{
              core::CapabilityId(std::string(id)),
              core::CapabilityDefinitionRevision(std::string(descriptor->capability_definition_revision)),
              core::ProgramCapabilityDisposition::kRequired};
          const auto existing = std::find_if(it->second.required_capabilities.begin(),
              it->second.required_capabilities.end(), [&](const auto& row) {
                return row.capability_id == requested.capability_id;
              });
          if (existing == it->second.required_capabilities.end())
            it->second.required_capabilities.push_back(requested);
          else if (existing->capability_definition_revision != requested.capability_definition_revision)
            fail("pneumatic capability conflicts with the Program capability revision");
          else existing->disposition = core::ProgramCapabilityDisposition::kRequired;
        };
        if (signal.direction == core::IoSignalDirection::kOutput)
          add_capability("io.gpio.set_output");
        else
          add_capability("io.gpio.get_input");
      }
    }
    for (const auto& requirement : program_contract.target_requirements) {
      if (!requirement.default_tcp_required) continue;
      const auto entry = resolve_direct_default_tcp(requirement.target.component_id,
                                                     retained_model_map);
      if (!entry) continue;
      if (collector)
        collector->derived_dependents[entry->target].insert(requirement.target);
      auto [it, inserted] = requirements.try_emplace(entry->target,
          core::ProgramTargetRequirementDTO{entry->target, {}, {}, {}, {}});
      (void)inserted;
      it->second.frame_references.push_back(entry->frame);
      contract.frame_requirements.push_back(entry->frame);
    }
    const auto unique_sort = [](auto& rows) {
      std::sort(rows.begin(), rows.end());
      rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    };
    unique_sort(contract.signal_requirements);
    unique_sort(contract.frame_requirements);
    contract.target_requirements.clear();
    for (auto& [target, requirement] : requirements) {
      (void)target;
      unique_sort(requirement.signal_references);
      unique_sort(requirement.frame_references);
      std::sort(requirement.required_capabilities.begin(), requirement.required_capabilities.end(),
          [](const auto& left, const auto& right) { return left.capability_id < right.capability_id; });
      contract.target_requirements.push_back(std::move(requirement));
    }
    for (const auto& requirement : contract.target_requirements) {
      try {
        const auto& component = require_component(bundle.cell_graph, requirement.target.component_id);
        const auto type = load_component_type(bundle, component, catalog.capabilities, catalog_artifact_store);
        (void)require_target_type(type, requirement.target.command_target_id);
      } catch (const TargetUnavailable& error) {
        if (!collector) throw;
        std::string detail(error.what());
        detail.resize(std::min(detail.size(), kMaximumProgramBindingUnresolvedDetailBytes));
        collector->rows.push_back({requirement.target, error.reason(), std::move(detail)});
      }
    }
    if (collector && !collector->rows.empty()) {
      for (const auto& reference : contract.frame_requirements) {
        auto matches = find_program_frame_matches(reference, retained_model_map);
        if (matches.size() == 1U) collector->resolved.frame_bindings.push_back(std::move(matches.front()));
      }
      return nullptr;
    }
    auto references = resolve_direct_reference_bindings(contract, retained_model_map, io.value());
    const auto program_components = referenced_program_components(program_contract);
    for (auto& frame : component_frame_closure(program_components, retained_model_map)) {
      const auto existing = std::find_if(references.frames.begin(), references.frames.end(),
          [&](const auto& row) { return row.reference == frame.reference; });
      if (existing == references.frames.end()) references.frames.push_back(std::move(frame));
      else if (*existing != frame) fail("component frame closure conflicts with an authored reference");
    }
    std::sort(references.frames.begin(), references.frames.end(),
        [](const auto& left, const auto& right) { return left.reference < right.reference; });
    auto component_ids = referenced_program_components(contract);
    auto components = resolve_direct_components(component_ids, bundle, catalog, catalog_artifact_store);
    auto targets = resolve_direct_targets(contract, bundle, catalog, retained_model_map,
                                          components, references, io.value(),
                                          intrinsic_resources, collector);
    if (collector && !collector->rows.empty()) return nullptr;
    const auto merge_rows = [](auto& destination, auto additions) {
      for (auto& row : additions) {
        const auto existing = std::find_if(destination.begin(), destination.end(),
            [&](const auto& value) { return value.reference == row.reference; });
        if (existing == destination.end()) destination.push_back(std::move(row));
        else if (*existing != row) fail("derived Program binding facts conflict");
      }
      std::sort(destination.begin(), destination.end(),
          [](const auto& left, const auto& right) { return left.reference < right.reference; });
    };
    for (auto& target : targets) {
      if (const auto route = routes.find(target.target); route != routes.end())
        target.pneumatic_io = route->second;
      target.default_tcp = resolve_direct_default_tcp(target.target.component_id,
                                                       retained_model_map);
      const bool motion = std::any_of(target.capability_bindings.begin(), target.capability_bindings.end(),
          [](const auto& row) { return row.capability_id.value() == "execution.trajectory.execute"; });
      if (!motion) continue;
      auto tcps = resolve_direct_tcp_bindings(target.target, bundle, retained_model_map,
                                               catalog.capabilities, catalog_artifact_store);
      for (const auto& frame : tcps.frames) component_ids.insert(frame.reference.component_id);
      for (const auto& body : tcps.contact_bodies) component_ids.insert(body.reference.component_id);
      merge_rows(references.frames, std::move(tcps.frames));
      merge_rows(references.bodies, std::move(tcps.contact_bodies));
      target.robot_tcps = std::move(tcps.tcps);
      target.default_tcp = std::move(tcps.default_tcp);
    }
    components = resolve_direct_components(component_ids, bundle, catalog, catalog_artifact_store);
    return seal_direct_program_binding(contract, retained_bundle, retained_model_map,
                                        catalog_artifact_store, std::move(components.components), std::move(targets),
                                        std::move(references));
  } catch (const ProgramBindingCompilerError&) {
    throw;
  } catch (const TargetUnavailable& error) {
    fail(error.what());
  } catch (const std::exception& error) {
    fail(std::string("Program Binding compilation failed: ") + error.what());
  }
}

template <typename StrongId>
[[nodiscard]] std::vector<StrongId> sorted_ids(
    const std::set<std::string>& values) {
  std::vector<StrongId> result;
  result.reserve(values.size());
  for (const auto& value : values) {
    result.emplace_back(value);
  }
  return result;
}

template <typename StrongId>
[[nodiscard]] CanonicalValue strong_id_array(
    const std::vector<StrongId>& values) {
  CanonicalValue::Array result;
  result.reserve(values.size());
  for (const auto& value : values) {
    result.emplace_back(value.value());
  }
  return CanonicalValue(std::move(result));
}

[[nodiscard]] const core::ComponentTypeCapabilityDTO*
find_target_capability(const core::ComponentTypeCommandTargetDTO& target,
                       const std::string_view capability_id) {
  return find_exactly_one<core::ComponentTypeCapabilityDTO>(
      target.capabilities, [&](const auto& capability) {
        return capability.capability_id.value() == capability_id;
      });
}


// The tool on a robot's `flange` interface, its TCP target (the
// target of its default TCP) and that target's TCP roles.
struct DirectControlMountedTool final {
  core::ControllableComponentId controllable_component_id;
  core::CommandTargetId command_target_id;
  std::set<std::string> model_roles;
  std::set<std::string> tcp_roles;
};

[[nodiscard]] CanonicalValue direct_target_projection(
    const core::ProgramTargetReferenceDTO& target) {
  return CanonicalValue(CanonicalValue::Object{
      {"component_id", CanonicalValue(target.component_id.value())},
      {"command_target_id", CanonicalValue(target.command_target_id.value())}});
}

void require_direct_control_model_roles(
    const core::ControllableComponentDTO& component,
    const std::set<std::string>& required_roles) {
  for (const auto& role : required_roles) {
    if (std::none_of(component.model_role_bindings.begin(), component.model_role_bindings.end(),
                     [&](const auto& binding) { return binding.model_role_id.value() == role; }))
      fail("direct-control target requires an unavailable component model role");
  }
}

[[nodiscard]] std::set<core::ResourceRoleId> direct_control_resource_roles(
    const core::ComponentTypeCommandTargetDTO& target) {
  std::set<core::ResourceRoleId> result;
  for (const auto& resource : target.resource_roles)
    result.insert(core::resource_role_id(resource));
  return result;
}

[[nodiscard]] std::optional<DirectControlMountedTool> direct_control_mounted_tool(
    const core::RetainedBundleDTO& bundle,
    const VerifiedRetainedModelMap& retained_model_map,
    const core::ControllableComponentId& robot,
    std::span<const QualifiedCapabilityDefinition> capabilities,
    const ICatalogArtifactSource& catalog_artifact_store) {
  // The single or primary tool on the `flange` interface.
  const auto& index = retained_model_map.kinematic_index();
  const auto* tool_id = index.tool_on(robot, "flange");
  if (tool_id == nullptr) return std::nullopt;
  // A tool without a default TCP has no TCP.
  const auto* default_tcp = index.default_tcp_of(*tool_id);
  if (default_tcp == nullptr) return std::nullopt;
  const auto& tool = require_component(bundle.cell_graph, *tool_id);
  const auto type =
      load_component_type(bundle, tool, capabilities, catalog_artifact_store);
  const auto& target = require_target_type(type, default_tcp->command_target_id);
  DirectControlMountedTool result{tool.controllable_component_id,
                                  target.command_target_id, {}, {}};
  for (const auto& frame : target.frame_roles) {
    result.model_roles.insert(frame.model_role_id.value());
  }
  require_direct_control_model_roles(tool, result.model_roles);
  result.tcp_roles = target_tcp_roles(
      target, retained_model_map.find_target(tool.controllable_component_id,
                                             target.command_target_id), false);
  return result;
}

[[nodiscard]] core::ProgramBindingContractDTO
make_direct_control_motion_contract(
    const core::RetainedNodeEnvironmentDTO& node_environment,
    const core::ControllableComponentDTO& component,
    const core::ComponentTypeCommandTargetDTO& target,
    const std::set<std::string>& tcp_roles,
    const std::optional<DirectControlMountedTool>& tool) {
  std::set<std::string> model_role_names;
  for (const auto& group : target.model_group_roles) {
    model_role_names.insert(group.model_role_id.value());
  }
  for (const auto& resource : target.resource_roles) {
    if (const auto* model =
            std::get_if<core::ComponentTypeModelResourceRoleDTO>(&resource)) {
      model_role_names.insert(model->model_role_id.value());
    }
  }
  for (const auto& frame : target.frame_roles) {
    model_role_names.insert(frame.model_role_id.value());
  }
  for (const auto& body : target.collision_body_roles) {
    model_role_names.insert(body.model_role_id.value());
  }
  if (model_role_names.empty() || target.frame_roles.empty() ||
      target.resource_roles.empty() || target.collision_body_roles.empty()) {
    fail("direct-control motion target has no complete model/frame/resource/"
         "collision contract");
  }
  require_direct_control_model_roles(component, model_role_names);
  const auto declares = [&](std::string_view id) {
    return std::any_of(target.frame_roles.begin(), target.frame_roles.end(),
                       [&](const auto& role) {
                         return role.frame_role_id.value() == id;
                       });
  };
  if (!declares(kDirectControlBaseFrameRoleId)) {
    fail("direct-control motion target omits its logical " +
         std::string(kDirectControlBaseFrameRoleId) + " frame role");
  }

  const auto model_roles =
      sorted_ids<core::ModelRoleId>(model_role_names);
  // Interim legacy endpoint binding: keep `base` and the current robot
  // plus mounted-tool TCP roles. The full Deployment inventory is compiled
  // separately into `robot_tcps`; other frame roles, a merely named `tcp`
  // included, are not endpoint bindings.
  std::set<std::string> frame_role_names{tcp_roles};
  frame_role_names.insert(std::string(kDirectControlBaseFrameRoleId));
  std::set<std::string> resource_role_names;
  for (const auto& resource : target.resource_roles) {
    resource_role_names.insert(core::resource_role_id(resource).value());
  }
  std::set<std::string> collision_role_names;
  for (const auto& body : target.collision_body_roles) {
    collision_role_names.insert(body.collision_body_role_id.value());
  }
  const auto frame_roles =
      sorted_ids<core::FrameRoleId>(frame_role_names);
  const auto resource_roles =
      sorted_ids<core::ResourceRoleId>(resource_role_names);
  const auto collision_roles =
      sorted_ids<core::CollisionBodyRoleId>(collision_role_names);
  const core::ProgramTargetReferenceDTO direct_target{
      component.controllable_component_id, target.command_target_id};

  if (find_target_capability(target, "execution.trajectory.execute") ==
          nullptr ||
      find_target_capability(target, "planning.motion.joint_goal") ==
          nullptr) {
    fail("direct-control motion target omits a required execution or "
         "joint-planning capability");
  }
  std::vector<core::ProgramCapabilityRequirementDTO> capabilities;
  capabilities.reserve(target.capabilities.size());
  for (const auto& capability : target.capabilities) {
    capabilities.push_back(core::ProgramCapabilityRequirementDTO{
        capability.capability_id,
        capability.capability_definition_revision,
        core::ProgramCapabilityDisposition::kRequired});
  }
  std::sort(capabilities.begin(), capabilities.end(),
            [](const auto& left, const auto& right) {
              return std::tie(left.capability_id,
                              left.capability_definition_revision) <
                     std::tie(right.capability_id,
                              right.capability_definition_revision);
            });

  CanonicalValue::Array capability_projection;
  capability_projection.reserve(capabilities.size());
  for (const auto& capability : capabilities) {
    capability_projection.emplace_back(CanonicalValue::Object{
        {"capability_definition_revision",
         CanonicalValue(
             capability.capability_definition_revision.value())},
        {"capability_id",
         CanonicalValue(capability.capability_id.value())},
        {"disposition", CanonicalValue("required")},
    });
  }
  const CanonicalValue projection(CanonicalValue::Object{
      {"api_version",
       CanonicalValue("manyforge.direct_control_motion_program.v3")},
      {"target", direct_target_projection(direct_target)},
      {"model_roles", strong_id_array(model_roles)},
      {"node_environment",
       CanonicalValue(CanonicalValue::Object{
           {"catalog_digest",
            CanonicalValue(node_environment.catalog_digest.value())},
           {"catalog_id", CanonicalValue(node_environment.catalog_id)},
           {"content_version",
            CanonicalValue(node_environment.content_version)},
           {"environment_digest",
            CanonicalValue(node_environment.environment_digest.value())},
       })},
      {"program_id",
       CanonicalValue(std::string(kDirectControlMotionProgramId))},
      {"target_requirements",
       CanonicalValue(CanonicalValue::Object{
           {"capabilities",
            CanonicalValue(std::move(capability_projection))},
           {"collision_body_roles", strong_id_array(collision_roles)},
           {"frame_roles", strong_id_array(frame_roles)},
           {"resource_roles", strong_id_array(resource_roles)},
       })},
  });
  // A mounted tool's TCPs retain their exact component and target identity.
  std::vector<core::FrameRoleId> tool_frame_roles;
  std::vector<core::ModelRoleId> tool_model_roles;
  auto revision_projection = projection;
  if (tool.has_value()) {
    tool_frame_roles = sorted_ids<core::FrameRoleId>(tool->tcp_roles);
    tool_model_roles = sorted_ids<core::ModelRoleId>(tool->model_roles);
    auto fields = projection.as_object();
    fields.emplace_back(
        "mounted_tool",
        CanonicalValue(CanonicalValue::Object{
            {"target", direct_target_projection(
                {tool->controllable_component_id, tool->command_target_id})},
            {"frame_roles", strong_id_array(tool_frame_roles)},
            {"model_roles", strong_id_array(tool_model_roles)},
        }));
    revision_projection = CanonicalValue(std::move(fields));
  }
  std::string revision_preimage{
      "manyforge.direct-control-motion-program-revision.v3"};
  revision_preimage.push_back('\0');
  revision_preimage.append(canonical_json(revision_projection));

  std::vector<core::ProgramFrameReferenceDTO> frames;
  for (const auto& role : frame_roles)
    frames.push_back({component.controllable_component_id, role});
  std::vector<core::ProgramCollisionBodyReferenceDTO> bodies;
  for (const auto& role : collision_roles)
    bodies.push_back({component.controllable_component_id, role});
  core::ProgramBindingContractDTO contract{
      core::ProgramId(std::string(kDirectControlMotionProgramId)),
      core::ProgramRevision(sha256_digest(revision_preimage)), node_environment,
      {{direct_target, std::move(capabilities), frames, bodies, {}}},
      frames, {}, bodies};
  if (tool.has_value()) {
    const core::ProgramTargetReferenceDTO tool_target{
        tool->controllable_component_id, tool->command_target_id};
    std::vector<core::ProgramFrameReferenceDTO> tool_frames;
    for (const auto& role : tool_frame_roles)
      tool_frames.push_back({tool->controllable_component_id, role});
    contract.target_requirements.push_back({tool_target, {}, tool_frames, {}, {}});
    contract.frame_requirements.insert(contract.frame_requirements.end(), tool_frames.begin(), tool_frames.end());
    std::sort(contract.target_requirements.begin(), contract.target_requirements.end(),
              [](const auto& left, const auto& right) { return left.target < right.target; });
    std::sort(contract.frame_requirements.begin(), contract.frame_requirements.end());
  }
  return contract;
}

[[nodiscard]] core::ProgramBindingContractDTO make_direct_control_io_contract(
    const core::RetainedNodeEnvironmentDTO &node_environment,
    const core::ProgramTargetReferenceDTO &direct_target,
    const core::ComponentTypeCommandTargetDTO &target,
    const std::vector<core::ResourceRoleId> &resource_roles,
    const std::vector<core::ProgramSignalReferenceDTO> &signals,
    bool include_inputs, bool include_outputs) {
  const auto *capability = find_target_capability(
      target, include_inputs ? "io.gpio.get_input" : "io.gpio.set_output");
  const auto *output =
      include_outputs ? find_target_capability(target, "io.gpio.set_output")
                      : nullptr;
  if (!capability || (include_outputs && !output) || resource_roles.empty() || signals.empty())
    fail("direct-control input target has no qualified input-read contract");
  CanonicalValue::Array signal_projection;
  for (const auto& signal : signals)
    signal_projection.emplace_back(CanonicalValue::Object{
        {"component_id", CanonicalValue(signal.component_id.value())},
        {"signal_name", CanonicalValue(signal.signal_name)}});
  auto projection_fields = CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.direct_control_io_program.v2")},
      {"program_id", CanonicalValue(std::string(kDirectControlIoProgramId))},
      {"target", direct_target_projection(direct_target)},
      {"capability_id", CanonicalValue(capability->capability_id.value())},
      {"capability_definition_revision",
       CanonicalValue(capability->capability_definition_revision.value())},
      {"resource_roles", strong_id_array(resource_roles)},
      {"signals", CanonicalValue(std::move(signal_projection))},
      {"node_environment",
       CanonicalValue(CanonicalValue::Object{
           {"catalog_id", CanonicalValue(node_environment.catalog_id)},
           {"content_version",
            CanonicalValue(node_environment.content_version)},
           {"catalog_digest",
            CanonicalValue(node_environment.catalog_digest.value())},
           {"environment_digest",
            CanonicalValue(node_environment.environment_digest.value())}})}};
  std::vector<core::ProgramCapabilityRequirementDTO> capabilities{
      {capability->capability_id, capability->capability_definition_revision,
       core::ProgramCapabilityDisposition::kRequired}};
  if (include_inputs && include_outputs) {
    projection_fields.emplace_back(
        "output_capability_id", CanonicalValue(output->capability_id.value()));
    projection_fields.emplace_back(
        "output_capability_definition_revision",
        CanonicalValue(output->capability_definition_revision.value()));
    capabilities.push_back({output->capability_id,
                            output->capability_definition_revision,
                            core::ProgramCapabilityDisposition::kRequired});
  }
  const CanonicalValue projection(std::move(projection_fields));
  std::string preimage("manyforge.direct-control-io-program-revision.v2");
  preimage.push_back('\0');
  preimage.append(canonical_json(projection));
  return {core::ProgramId(std::string(kDirectControlIoProgramId)),
          core::ProgramRevision(sha256_digest(preimage)),
          node_environment,
          {{direct_target, std::move(capabilities), {}, {}, signals}},
          {}, signals, {}};
}

[[nodiscard]] core::ProgramBindingContractDTO
make_direct_control_gripper_contract(
    const core::RetainedNodeEnvironmentDTO &node_environment,
    const core::ControllableComponentDTO &component,
    const core::ComponentTypeCommandTargetDTO &target) {
  const auto *command_capability =
      find_target_capability(target, "tool.gripper.command");
  if (command_capability == nullptr) {
    fail("direct-control gripper target omits tool.gripper.command");
  }

  std::set<std::string> model_role_names;
  for (const auto& group : target.model_group_roles) {
    model_role_names.insert(group.model_role_id.value());
  }
  for (const auto& resource : target.resource_roles) {
    if (const auto* model =
            std::get_if<core::ComponentTypeModelResourceRoleDTO>(&resource)) {
      model_role_names.insert(model->model_role_id.value());
    }
  }
  for (const auto& frame : target.frame_roles) {
    model_role_names.insert(frame.model_role_id.value());
  }
  for (const auto& body : target.collision_body_roles) {
    model_role_names.insert(body.model_role_id.value());
  }
  const bool model_backed_resource =
      target.resource_roles.size() == 1U &&
      std::holds_alternative<core::ComponentTypeModelResourceRoleDTO>(
          target.resource_roles.front());
  if (model_role_names.empty() || target.resource_roles.size() != 1U ||
      (model_backed_resource && target.model_group_roles.size() != 1U) ||
      (!model_backed_resource && !target.model_group_roles.empty())) {
    fail("direct-control gripper target has no exact model/group/resource "
         "contract");
  }
  require_direct_control_model_roles(component, model_role_names);

  const auto model_roles = sorted_ids<core::ModelRoleId>(model_role_names);
  std::set<std::string> frame_role_names;
  for (const auto& frame : target.frame_roles) {
    frame_role_names.insert(frame.frame_role_id.value());
  }
  std::set<std::string> resource_role_names;
  for (const auto& resource : target.resource_roles) {
    resource_role_names.insert(core::resource_role_id(resource).value());
  }
  std::set<std::string> collision_role_names;
  for (const auto& body : target.collision_body_roles) {
    collision_role_names.insert(body.collision_body_role_id.value());
  }
  const auto frame_roles = sorted_ids<core::FrameRoleId>(frame_role_names);
  const auto resource_roles =
      sorted_ids<core::ResourceRoleId>(resource_role_names);
  const auto collision_roles =
      sorted_ids<core::CollisionBodyRoleId>(collision_role_names);
  const core::ProgramTargetReferenceDTO direct_target{
      component.controllable_component_id, target.command_target_id};
  std::vector<core::ProgramCapabilityRequirementDTO> capabilities;
  capabilities.reserve(target.capabilities.size());
  for (const auto& capability : target.capabilities) {
    capabilities.push_back(core::ProgramCapabilityRequirementDTO{
        capability.capability_id,
        capability.capability_definition_revision,
        core::ProgramCapabilityDisposition::kRequired});
  }
  std::sort(capabilities.begin(), capabilities.end(),
            [](const auto& left, const auto& right) {
              return std::tie(left.capability_id,
                              left.capability_definition_revision) <
                     std::tie(right.capability_id,
                              right.capability_definition_revision);
            });

  CanonicalValue::Array capability_projection;
  capability_projection.reserve(capabilities.size());
  for (const auto& capability : capabilities) {
    capability_projection.emplace_back(CanonicalValue::Object{
        {"capability_definition_revision",
         CanonicalValue(
             capability.capability_definition_revision.value())},
        {"capability_id", CanonicalValue(capability.capability_id.value())},
        {"disposition", CanonicalValue("required")},
    });
  }

  const CanonicalValue projection(CanonicalValue::Object{
      {"api_version",
       CanonicalValue("manyforge.direct_control_gripper_program.v3")},
      {"target", direct_target_projection(direct_target)},
      {"model_roles", strong_id_array(model_roles)},
      {"node_environment",
       CanonicalValue(CanonicalValue::Object{
           {"catalog_digest",
            CanonicalValue(node_environment.catalog_digest.value())},
           {"catalog_id", CanonicalValue(node_environment.catalog_id)},
           {"content_version",
            CanonicalValue(node_environment.content_version)},
           {"environment_digest",
            CanonicalValue(node_environment.environment_digest.value())},
       })},
      {"program_id",
       CanonicalValue(std::string(kDirectControlGripperProgramId))},
      {"target_requirements",
       CanonicalValue(CanonicalValue::Object{
           {"capabilities",
            CanonicalValue(std::move(capability_projection))},
           {"collision_body_roles", strong_id_array(collision_roles)},
           {"frame_roles", strong_id_array(frame_roles)},
           {"resource_roles", strong_id_array(resource_roles)},
       })},
  });
  std::string revision_preimage{
      "manyforge.direct-control-gripper-program-revision.v3"};
  revision_preimage.push_back('\0');
  revision_preimage.append(canonical_json(projection));

  std::vector<core::ProgramFrameReferenceDTO> frames;
  for (const auto& role : frame_roles)
    frames.push_back({component.controllable_component_id, role});
  std::vector<core::ProgramCollisionBodyReferenceDTO> bodies;
  for (const auto& role : collision_roles)
    bodies.push_back({component.controllable_component_id, role});
  return core::ProgramBindingContractDTO{
      core::ProgramId(std::string(kDirectControlGripperProgramId)),
      core::ProgramRevision(sha256_digest(revision_preimage)), node_environment,
      {{direct_target, std::move(capabilities), frames, bodies, {}}},
      frames, {}, bodies};
}

}  // namespace

core::ProgramSignalBindingDTO resolve_program_signal_binding(
    const core::ProgramSignalReferenceDTO &reference,
    std::span<const RetainedIoSignalConfiguration> signals) {
  const auto matches = FindProgramSignalMatches(signals, reference);
  if (matches.size() != 1U)
    fail("Program signal reference is missing or ambiguous");
  const auto &match = matches.front();
  const auto digest = core::ComputeIoSignalDescriptorDigest(match.descriptor);
  if (!digest.ok()) fail("Program signal descriptor cannot be digested");
  return {reference,
          {reference.component_id, match.retained->command_target_id},
          match.retained->runtime_unit_id,
          match.descriptor.value_type,
          match.descriptor.direction,
          match.descriptor.transition_capability,
          digest.value()};
}

std::vector<RobotTcp> robot_tcps(
    const RetainedBundle& retained_bundle,
    const VerifiedRetainedModelMap& retained_model_map,
    const core::ControllableComponentId& robot,
    const core::CommandTargetId& robot_target,
    std::span<const QualifiedCapabilityDefinition> capabilities,
    const ICatalogArtifactSource& catalog_artifact_store) {
  if (!verify_retained_bundle_identity(retained_bundle) ||
      retained_model_map.query().retained_bundle_id !=
          retained_bundle.retained_bundle_id()) {
    fail("robot TCP list inputs do not share one verified retained bundle");
  }
  return compile_robot_tcps(retained_bundle.value(), retained_model_map, robot,
                            robot_target, capabilities,
                            catalog_artifact_store);
}

std::vector<RobotTcp> qualified_candidate_robot_tcps(
    const core::ProgramTargetReferenceDTO& target,
    const RetainedBundle& retained_bundle,
    const VerifiedRetainedModelMap& retained_model_map,
    const ICatalogArtifactSource& catalog_artifact_store) {
  if (!verify_retained_bundle_identity(retained_bundle) ||
      retained_model_map.query().retained_bundle_id != retained_bundle.retained_bundle_id())
    fail("candidate TCP inputs do not share one verified retained bundle");
  const auto catalog = load_qualified_catalog_closure(retained_bundle.value(), catalog_artifact_store);
  return compile_robot_tcps(retained_bundle.value(), retained_model_map,
                            target.component_id, target.command_target_id,
                            catalog.capabilities, catalog_artifact_store);
}

std::shared_ptr<const ProgramBinding> compile_program_binding(
    const ProgramBindingContract& program_contract,
    const RetainedBundle& retained_bundle,
    const VerifiedRetainedModelMap& retained_model_map,
    const ICatalogArtifactSource& catalog_artifact_store) {
  return compile_program_binding_contract(
      program_contract.value(), retained_bundle, retained_model_map,
      catalog_artifact_store);
}

ProgramBindingOutcome compile_program_binding_outcome(
    const ProgramBindingContract& program_contract,
    const RetainedBundle& retained_bundle,
    const VerifiedRetainedModelMap& retained_model_map,
    const ICatalogArtifactSource& catalog_artifact_store) {
  DirectTargetCollector collector;
  auto binding = compile_program_binding_contract(
      program_contract.value(), retained_bundle, retained_model_map,
      catalog_artifact_store, nullptr, &collector);
  if (binding != nullptr) {
    if (!collector.rows.empty()) {
      fail("Program Binding outcome is both bound and unresolved");
    }
    return {std::move(binding), std::nullopt, std::nullopt};
  }
  if (collector.rows.empty()) {
    fail("Program Binding outcome has neither a binding nor a reason");
  }
  std::sort(collector.rows.begin(), collector.rows.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.target < rhs.target; });
  std::set<core::ProgramTargetReferenceDTO> affected_targets;
  std::vector<core::ProgramTargetReferenceDTO> pending;
  for (const auto& row : collector.rows)
    if (affected_targets.insert(row.target).second) pending.push_back(row.target);
  for (std::size_t index = 0; index < pending.size(); ++index) {
    const auto dependents = collector.derived_dependents.find(pending[index]);
    if (dependents == collector.derived_dependents.end()) continue;
    for (const auto& target : dependents->second)
      if (affected_targets.insert(target).second) pending.push_back(target);
  }
  const std::vector<core::ProgramTargetReferenceDTO> targets(
      affected_targets.begin(), affected_targets.end());
  auto affected = program_binding_affected_scope(
      program_contract.execution_table(), targets);
  const auto& bundle = retained_bundle.value();
  ProgramNonExecutableOutcomeDTO outcome{
      program_contract.value().program_id,
      program_contract.value().program_revision,
      retained_bundle.retained_bundle_id(),
      bundle.cell_graph.graph_revision,
      std::move(collector.rows),
      std::move(affected.nodes),
      std::move(affected.execution_groups),
  };
  if (!outcome.valid()) {
    fail("Program Binding produced an invalid non-executable outcome");
  }
  return {nullptr, std::move(outcome), std::move(collector.resolved)};
}

ProgramBindingOutcome compile_program_binding_outcome(
    const ProgramBindingContract& program_contract,
    const RetainedBundle& retained_bundle,
    const std::optional<VerifiedRetainedModelMap>& retained_model_map,
    const ICatalogArtifactSource& catalog_artifact_store) {
  if (retained_model_map.has_value()) {
    return compile_program_binding_outcome(
        program_contract, retained_bundle,
        *retained_model_map, catalog_artifact_store);
  }

  const auto& bundle = retained_bundle.value();
  if (!verify_retained_bundle_identity(retained_bundle) ||
      !bundle.compiled_products.symbol_role_maps.empty() ||
      !bundle.cell_graph.controllable_components.empty() ||
      !bundle.cell_graph.model_instances.empty()) {
    fail("an absent model map is valid only for an empty candidate cell");
  }
  if (program_contract.value().node_environment !=
      bundle.catalogs.node_environment) {
    fail("Program contract selects another retained node environment");
  }
  // Keep the same strict retained-catalog qualification as the ordinary
  // compiler. Missing references may become a named outcome; corrupt or
  // ambiguous candidate facts still abort preparation.
  static_cast<void>(load_qualified_catalog_closure(
      bundle, catalog_artifact_store));
  static_cast<void>(detail::BuildQualifiedRetentionAuthority::qualify());

  DirectTargetCollector collector;
  for (const auto& requirement : program_contract.value().target_requirements) {
    try {
      static_cast<void>(require_component(
          bundle.cell_graph, requirement.target.component_id));
      // A component without its required retained model map is not an
      // intentional candidate-dependent absence.
      fail("candidate component has no exact retained model map");
    } catch (const TargetUnavailable& error) {
      std::string detail(error.what());
      detail.resize(std::min(detail.size(),
                             kMaximumProgramBindingUnresolvedDetailBytes));
      collector.rows.push_back(ProgramBindingUnresolvedTargetDTO{
          requirement.target, error.reason(), std::move(detail)});
    }
  }
  if (collector.rows.empty()) {
    fail("an empty candidate cannot bind a Program without model data");
  }
  std::sort(collector.rows.begin(), collector.rows.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.target < rhs.target; });
  std::vector<core::ProgramTargetReferenceDTO> targets;
  for (const auto& row : collector.rows) targets.push_back(row.target);
  const auto affected = program_binding_affected_scope(
      program_contract.execution_table(), targets);
  ProgramNonExecutableOutcomeDTO outcome{
      program_contract.value().program_id,
      program_contract.value().program_revision,
      retained_bundle.retained_bundle_id(),
      bundle.cell_graph.graph_revision,
      std::move(collector.rows),
      affected.nodes,
      affected.execution_groups,
  };
  if (!outcome.valid()) {
    fail("Program Binding produced an invalid non-executable outcome");
  }
  return {nullptr, std::move(outcome), ProgramResolvedReferencesDTO{}};
}

std::shared_ptr<const ProgramBinding>
compile_direct_control_motion_program_binding(
    const core::ControllableComponentId& controllable_component_id,
    const core::CommandTargetId& command_target_id,
    const RetainedBundle& retained_bundle,
    const VerifiedRetainedModelMap& retained_model_map,
    const ICatalogArtifactSource& catalog_artifact_store) {
  try {
    if (!verify_retained_bundle_identity(retained_bundle) ||
        retained_model_map.query().retained_bundle_id !=
            retained_bundle.retained_bundle_id()) {
      fail("direct-control compiler inputs do not share one verified retained "
           "bundle");
    }
    const auto& bundle = retained_bundle.value();
    const auto& component = require_component(
        bundle.cell_graph, controllable_component_id);
    const auto catalog = load_qualified_catalog_closure(
        bundle, catalog_artifact_store);
    const auto component_type = load_component_type(
        bundle, component, catalog.capabilities, catalog_artifact_store);
    const auto& target = require_target_type(component_type, command_target_id);
    const auto tool = direct_control_mounted_tool(
        bundle, retained_model_map, controllable_component_id,
        catalog.capabilities, catalog_artifact_store);
    auto contract = make_direct_control_motion_contract(
        bundle.catalogs.node_environment, component, target,
        target_tcp_roles(target, retained_model_map.find_target(
                                     controllable_component_id,
                                     command_target_id), false),
        tool);
    const DirectControlResourceRequirements resources{
        {{controllable_component_id, command_target_id}, direct_control_resource_roles(target)}};
    return compile_program_binding_contract(
        contract, retained_bundle, retained_model_map, catalog_artifact_store, &resources);
  } catch (const ProgramBindingCompilerError&) {
    throw;
  } catch (const std::exception& error) {
    fail(std::string("Direct-control Program Binding compilation failed: ") +
         error.what());
  }
}

std::shared_ptr<const ProgramBinding>
compile_direct_control_gripper_program_binding(
    const core::ControllableComponentId& controllable_component_id,
    const core::CommandTargetId& command_target_id,
    const RetainedBundle& retained_bundle,
    const VerifiedRetainedModelMap& retained_model_map,
    const ICatalogArtifactSource& catalog_artifact_store) {
  try {
    if (!verify_retained_bundle_identity(retained_bundle) ||
        retained_model_map.query().retained_bundle_id !=
            retained_bundle.retained_bundle_id()) {
      fail("direct-control compiler inputs do not share one verified retained "
           "bundle");
    }
    const auto& bundle = retained_bundle.value();
    const auto& component =
        require_component(bundle.cell_graph, controllable_component_id);
    const auto catalog = load_qualified_catalog_closure(
        bundle, catalog_artifact_store);
    const auto component_type = load_component_type(
        bundle, component, catalog.capabilities, catalog_artifact_store);
    const auto& target = require_target_type(component_type, command_target_id);
    auto contract = make_direct_control_gripper_contract(
        bundle.catalogs.node_environment, component, target);
    const DirectControlResourceRequirements resources{
        {{controllable_component_id, command_target_id}, direct_control_resource_roles(target)}};
    auto binding = compile_program_binding_contract(
        contract, retained_bundle, retained_model_map, catalog_artifact_store, &resources);
    const auto& targets = binding->value().target_bindings;
    const auto bound = std::find_if(targets.begin(), targets.end(), [&](const auto &row) {
      return row.target == core::ProgramTargetReferenceDTO{controllable_component_id, command_target_id};
    });
    if (bound == targets.end() ||
        bound->resource_role_bindings.size() != 1U ||
        bound->resource_role_bindings.front().resource_ids.size() !=
            1U) {
      fail("direct-control gripper target does not resolve to one exact "
           "command coordinate");
    }
    return binding;
  } catch (const ProgramBindingCompilerError&) {
    throw;
  } catch (const std::exception& error) {
    fail(std::string("Direct-control Program Binding compilation failed: ") +
         error.what());
  }
}

std::shared_ptr<const ProgramBinding> compile_direct_control_io_program_binding(
    const core::ControllableComponentId &controllable_component_id,
    const core::CommandTargetId &command_target_id,
    const RetainedBundle &retained_bundle,
    const VerifiedRetainedModelMap &retained_model_map,
    const ICatalogArtifactSource &catalog_artifact_store) {
  try {
    if (!verify_retained_bundle_identity(retained_bundle) ||
        retained_model_map.query().retained_bundle_id !=
            retained_bundle.retained_bundle_id())
      fail("direct-control input compiler inputs do not share one verified "
           "retained bundle");
    const auto &bundle = retained_bundle.value();
    const auto &component =
        require_component(bundle.cell_graph, controllable_component_id);
    const auto catalog =
        load_qualified_catalog_closure(bundle, catalog_artifact_store);
    const auto component_type = load_component_type(
        bundle, component, catalog.capabilities, catalog_artifact_store);
    const auto &target = require_target_type(component_type, command_target_id);
    const auto signals = ResolveRetainedIoSignalCatalog(bundle.cell_graph);
    if (!signals.ok())
      fail(signals.error().details.at("reason"));
    std::set<std::string> role_names;
    std::set<core::ProgramSignalReferenceDTO> signal_refs;
    bool include_inputs = false;
    bool include_outputs = false;
    for (const auto &signal : signals.value())
      if (signal.descriptor.signal_id.controllable_component_id ==
              controllable_component_id &&
          signal.command_target_id == command_target_id) {
        role_names.insert(signal.resource_role_id.value());
        include_inputs |=
            signal.descriptor.direction == core::IoSignalDirection::kInput;
        include_outputs |=
            signal.descriptor.direction == core::IoSignalDirection::kOutput;
        signal_refs.insert({controllable_component_id, signal.descriptor.signal_id.name});
      }
    const auto roles = sorted_ids<core::ResourceRoleId>(role_names);
    const std::vector<core::ProgramSignalReferenceDTO> references(signal_refs.begin(), signal_refs.end());
    const auto contract = make_direct_control_io_contract(
        bundle.catalogs.node_environment, {controllable_component_id, command_target_id},
        target, roles, references, include_inputs,
        include_outputs);
    return compile_program_binding_contract(
        contract, retained_bundle, retained_model_map, catalog_artifact_store);
  } catch (const ProgramBindingCompilerError &) {
    throw;
  } catch (const std::exception &error) {
    fail(std::string(
             "Direct-control input Program Binding compilation failed: ") +
         error.what());
  }
}

DirectControlProgramBindingInventory
compile_direct_control_program_binding_inventory(
    const RetainedBundle &retained_bundle,
    const VerifiedRetainedModelMap &retained_model_map,
    const ICatalogArtifactSource &catalog_artifact_store) {
  try {
    if (!verify_retained_bundle_identity(retained_bundle) ||
        retained_model_map.query().retained_bundle_id !=
            retained_bundle.retained_bundle_id()) {
      fail("direct-control inventory inputs do not share one verified "
           "retained bundle");
    }
    const auto& bundle = retained_bundle.value();
    const auto catalog = load_qualified_catalog_closure(
        bundle, catalog_artifact_store);
    DirectControlProgramBindingInventory result;
    for (const auto& component : bundle.cell_graph.controllable_components) {
      const auto component_type = load_component_type(
          bundle, component, catalog.capabilities, catalog_artifact_store);
      for (const auto& target : component_type.value().command_targets) {
        const bool has_execution =
            find_target_capability(target, "execution.trajectory.execute") !=
            nullptr;
        const bool has_joint_planning =
            find_target_capability(target, "planning.motion.joint_goal") !=
            nullptr;
        if (has_execution != has_joint_planning) {
          fail("direct-control motion target declares an incomplete execution/"
               "joint-planning capability family");
        }
        if (has_execution) {
          const auto tool = direct_control_mounted_tool(
              bundle, retained_model_map, component.controllable_component_id,
              catalog.capabilities, catalog_artifact_store);
          auto contract = make_direct_control_motion_contract(
              bundle.catalogs.node_environment, component, target,
              target_tcp_roles(target, retained_model_map.find_target(
                                           component.controllable_component_id,
                                           target.command_target_id), false),
              tool);
          const DirectControlResourceRequirements resources{
              {{component.controllable_component_id, target.command_target_id},
               direct_control_resource_roles(target)}};
          result.motion.push_back({{component.controllable_component_id, target.command_target_id},
              compile_program_binding_contract(
                  contract, retained_bundle, retained_model_map, catalog_artifact_store, &resources)});
        }
        if (find_target_capability(target, "io.gpio.get_input") != nullptr ||
            find_target_capability(target, "io.gpio.set_output") != nullptr) {
          result.io.push_back(compile_direct_control_io_program_binding(
              component.controllable_component_id, target.command_target_id,
              retained_bundle, retained_model_map, catalog_artifact_store));
        }
        if (find_target_capability(target, "tool.gripper.command") != nullptr) {
          auto contract = make_direct_control_gripper_contract(
              bundle.catalogs.node_environment, component, target);
          const DirectControlResourceRequirements resources{
              {{component.controllable_component_id, target.command_target_id},
               direct_control_resource_roles(target)}};
          result.gripper.push_back({{component.controllable_component_id, target.command_target_id},
              compile_program_binding_contract(
                  contract, retained_bundle, retained_model_map, catalog_artifact_store, &resources)});
        }
      }
    }
    return result;
  } catch (const ProgramBindingCompilerError &) {
    throw;
  } catch (const std::exception &error) {
    fail(std::string("Direct-control Program Binding inventory compilation "
                     "failed: ") +
         error.what());
  }
}

std::vector<DirectControlTcp> direct_control_motion_tcps(
    const ProgramBinding& direct_control_binding,
    const DirectControlProgramBindingInventory& inventory,
    const RetainedBundle& retained_bundle,
    const VerifiedRetainedModelMap& retained_model_map) {
  if (retained_model_map.query().retained_bundle_id != retained_bundle.retained_bundle_id() ||
      direct_control_binding.value().retained_bundle_id != retained_bundle.retained_bundle_id())
    fail("direct-control TCP projection inputs do not share one retained bundle");
  const DirectControlProgramBindingEntry *entry = nullptr;
  for (const auto &candidate : inventory.motion) {
    if (!candidate.binding || candidate.binding->program_binding_id() !=
                                  direct_control_binding.program_binding_id()) continue;
    if (entry) fail("direct-control motion binding has ambiguous inventory provenance");
    entry = &candidate;
  }
  if (!entry) fail("direct-control motion binding has no inventory provenance");
  const auto &targets = direct_control_binding.value().target_bindings;
  const auto *robot_row = find_exactly_one<core::ProgramTargetBindingDTO>(
      targets, [&](const auto &row) { return row.target == entry->target; });
  if (!robot_row) fail("direct-control motion target is missing or ambiguous");
  const auto &robot = *robot_row;
  if (std::none_of(robot.capability_bindings.begin(), robot.capability_bindings.end(),
                   [](const auto &capability) {
                     return capability.capability_id.value() == "execution.trajectory.execute";
                   }))
    fail("direct-control motion target lacks its execution capability");
  const auto& index = retained_model_map.kinematic_index();
  std::vector<DirectControlTcp> rows;
  if (!robot.robot_tcps) return rows;
  rows.reserve(robot.robot_tcps->size());
  for (const auto& tcp : *robot.robot_tcps) {
    const auto& owner = tcp.frame.reference.component_id;
    const bool mounted_here = index.carries(robot.target.component_id, owner);
    std::vector<core::ProgramTargetReferenceDTO> associated_targets;
    for (const auto& target : retained_model_map.targets()) {
      bool matches = false;
      for (const auto& candidate : target.frames) {
        const auto* frame = std::get_if<CompositionComponentModelFrame>(&candidate.frame);
        if (frame && frame->controllable_component_id == owner &&
            frame->frame_role_id == tcp.frame.reference.frame_id &&
            frame->model_instance_id == tcp.frame.model_instance_id &&
            frame->model_revision == tcp.frame.model_revision &&
            frame->compiled_frame_name == tcp.frame.compiled_frame_name)
          matches = true;
      }
      if (matches && index.carries(target.controllable_component_id, owner))
        associated_targets.push_back({target.controllable_component_id,
                                      target.command_target_id});
    }
    std::sort(associated_targets.begin(), associated_targets.end());
    associated_targets.erase(std::unique(associated_targets.begin(), associated_targets.end()),
                             associated_targets.end());
    std::vector<DirectControlTcpFrameAssociation> frame_associations;
    const auto collect_associations = [&](const auto& candidate) {
      if (!candidate) return;
      const auto& sealed = candidate->value();
      for (const auto& bound_target : sealed.target_bindings) {
        if (!std::binary_search(associated_targets.begin(), associated_targets.end(),
                                bound_target.target)) continue;
        for (const auto& association : bound_target.frame_associations) {
          if (association.frame != tcp.frame.reference) continue;
          const auto* geometry = find_exactly_one<core::ProgramFrameBindingDTO>(
              sealed.frame_bindings, [&](const auto& frame) {
                return frame.reference == association.frame;
              });
          if (!geometry || *geometry != tcp.frame)
            fail("direct-control TCP association has inconsistent sealed geometry");
          frame_associations.push_back({bound_target.target, association.frame,
                                        association.contact_body_role_ids});
        }
      }
    };
    for (const auto& candidate : inventory.motion) collect_associations(candidate.binding);
    for (const auto& candidate : inventory.gripper) collect_associations(candidate.binding);
    std::sort(frame_associations.begin(), frame_associations.end(),
              [](const auto& left, const auto& right) {
                return std::tie(left.target, left.frame) <
                       std::tie(right.target, right.frame);
              });
    for (std::size_t i = 1; i < frame_associations.size(); ++i) {
      const auto& previous = frame_associations[i - 1];
      const auto& current = frame_associations[i];
      if (previous.target == current.target && previous.frame == current.frame &&
          previous.contact_body_role_ids != current.contact_body_role_ids)
        fail("direct-control TCP association is ambiguous");
    }
    frame_associations.erase(
        std::unique(frame_associations.begin(), frame_associations.end(),
                    [](const auto& left, const auto& right) {
                      return left.target == right.target && left.frame == right.frame;
                    }),
        frame_associations.end());
    const bool mounted_elsewhere = !mounted_here && std::any_of(
        inventory.motion.begin(), inventory.motion.end(), [&](const auto& candidate) {
          if (!candidate.binding) return false;
          return candidate.target.component_id != robot.target.component_id &&
                 index.carries(candidate.target.component_id, owner);
        });
    rows.push_back(DirectControlTcp{tcp,
        owner.value() + "." + tcp.frame.reference.frame_id.value(),
        std::move(associated_targets), std::move(frame_associations),
        mounted_here, mounted_elsewhere});
  }
  std::sort(rows.begin(), rows.end(), [](const auto& left, const auto& right) {
    if (left.binding.end_effector != right.binding.end_effector)
      return left.binding.end_effector;
    return std::tie(left.binding.entry.target, left.binding.entry.frame) <
           std::tie(right.binding.entry.target, right.binding.entry.frame);
  });
  std::map<std::pair<std::string, std::string>, std::size_t> counts;
  for (const auto& row : rows)
    ++counts[{row.binding.frame.reference.component_id.value(),
              row.binding.frame.reference.frame_id.value()}];
  for (auto& row : rows)
    if (counts.at({row.binding.frame.reference.component_id.value(),
                   row.binding.frame.reference.frame_id.value()}) > 1U)
      row.display += " (" + row.binding.entry.target.command_target_id.value() + ")";
  return rows;
}

}  // namespace manyforge::deployment
