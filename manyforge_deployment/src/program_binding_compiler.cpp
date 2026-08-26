#include "manyforge_deployment/program_binding_compiler.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <span>
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
#include "manyforge_deployment/command_schema_codec.hpp"
#include "manyforge_deployment/component_type_codec.hpp"
#include "manyforge_deployment/content_identity.hpp"
#include "program_binding_internal.hpp"

namespace manyforge::deployment {
namespace {

constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";

[[noreturn]] void fail(std::string message) {
  throw ProgramBindingCompilerError(std::move(message));
}

template <typename Value, typename Predicate>
[[nodiscard]] const Value* find_exactly_one(std::span<const Value> values,
                                            Predicate predicate) {
  const Value* result = nullptr;
  for (const auto& value : values) {
    if (!predicate(value)) {
      continue;
    }
    if (result != nullptr) {
      return nullptr;
    }
    result = &value;
  }
  return result;
}

[[nodiscard]] const CanonicalValue::Object& require_object(
    const CanonicalValue& value, std::string_view path) {
  const auto* object = std::get_if<CanonicalValue::Object>(&value.storage());
  if (object == nullptr) {
    fail(std::string(path) + ": expected object while selecting dependencies");
  }
  return *object;
}

[[nodiscard]] const CanonicalValue::Array& require_array(
    const CanonicalValue& value, std::string_view path) {
  const auto* array = std::get_if<CanonicalValue::Array>(&value.storage());
  if (array == nullptr) {
    fail(std::string(path) + ": expected array while selecting dependencies");
  }
  return *array;
}

[[nodiscard]] const CanonicalValue& require_member(
    const CanonicalValue::Object& object, std::string_view name,
    std::string_view path) {
  const auto found = std::find_if(
      object.begin(), object.end(),
      [name](const auto& member) { return member.first == name; });
  if (found == object.end()) {
    fail(std::string(path) + ": missing '" + std::string(name) +
         "' while selecting dependencies");
  }
  return found->second;
}

[[nodiscard]] const std::string& require_string(
    const CanonicalValue& value, std::string_view path) {
  const auto* text = std::get_if<std::string>(&value.storage());
  if (text == nullptr) {
    fail(std::string(path) + ": expected string while selecting dependencies");
  }
  return *text;
}

template <typename Record>
[[nodiscard]] const Record& require_catalog_record(
    const core::RetainedBundleDTO& bundle, std::string_view id,
    std::string_view revision, std::string_view label) {
  const Record* exact = nullptr;
  bool conflicting_revision = false;
  for (const auto& candidate : bundle.catalogs.records) {
    const auto* typed = std::get_if<Record>(&candidate);
    if (typed == nullptr || typed->record_id.value() != id) {
      continue;
    }
    if (typed->record_revision.value() != revision) {
      conflicting_revision = true;
      continue;
    }
    if (exact != nullptr) {
      fail(std::string(label) + ": duplicate exact retained catalog record");
    }
    exact = typed;
  }
  if (exact == nullptr || conflicting_revision) {
    fail(std::string(label) +
         ": missing, stale, or ambiguous retained catalog record");
  }
  return *exact;
}

template <typename Record>
[[nodiscard]] std::string read_catalog_record(
    const Record& record, const ICatalogArtifactSource& store,
    std::string_view label) {
  auto bytes = store.get_verified(record.content_addressed_store_key,
                                  record.content_digest, std::nullopt);
  if (!bytes.has_value()) {
    fail(std::string(label) + ": exact retained catalog bytes are unavailable");
  }
  return std::move(*bytes);
}

struct QualifiedCatalogClosure final {
  std::vector<QualifiedCapabilityDefinition> capabilities;
  std::vector<QualifiedCommandSchema> command_schemas;
};

[[nodiscard]] QualifiedCatalogClosure load_qualified_catalog_closure(
    const core::RetainedBundleDTO& bundle,
    const ICatalogArtifactSource& store) {
  QualifiedCatalogClosure result;
  for (const auto& record : bundle.catalogs.records) {
    if (const auto* capability =
            std::get_if<core::CapabilityDefinitionCatalogRecordRefDTO>(
                &record);
        capability != nullptr) {
      const std::string bytes =
          read_catalog_record(*capability, store, "capability definition");
      result.capabilities.push_back(resolve_and_qualify_capability_definition(
          *capability, capability->content_addressed_store_key, bytes));
    } else if (const auto* command =
                   std::get_if<core::CommandSchemaCatalogRecordRefDTO>(
                       &record);
               command != nullptr) {
      const std::string bytes =
          read_catalog_record(*command, store, "command schema");
      result.command_schemas.push_back(resolve_and_qualify_command_schema(
          *command, command->content_addressed_store_key, bytes));
    }
  }
  return result;
}

[[nodiscard]] const QualifiedCapabilityDefinition*
find_qualified_capability(
    std::span<const QualifiedCapabilityDefinition> capabilities,
    std::string_view id, std::string_view revision) {
  return find_exactly_one<QualifiedCapabilityDefinition>(
      capabilities, [&](const auto& capability) {
        return capability.value().capability_id.value() == id &&
               capability.value().capability_definition_revision.value() ==
                   revision;
      });
}

[[nodiscard]] std::vector<QualifiedCapabilityDefinition>
select_component_capability_dependencies(
    std::string_view component_bytes,
    std::span<const QualifiedCapabilityDefinition> retained_capabilities) {
  const auto parsed = parse_strict_json_model_yaml(component_bytes);
  const auto& root = require_object(parsed.root(), "component_type");
  const auto& targets = require_array(
      require_member(root, "command_targets", "component_type"),
      "component_type.command_targets");
  std::set<std::pair<std::string, std::string>> selected_keys;
  std::vector<QualifiedCapabilityDefinition> selected;
  for (std::size_t target_index = 0U; target_index < targets.size();
       ++target_index) {
    const auto& target = require_object(
        targets[target_index], "component_type.command_targets[]");
    const auto& capabilities = require_array(
        require_member(target, "capabilities", "component_type target"),
        "component_type target.capabilities");
    for (const auto& candidate : capabilities) {
      const auto& capability =
          require_object(candidate, "component_type target capability");
      const std::string& id = require_string(
          require_member(capability, "capability_id", "component capability"),
          "component capability.capability_id");
      const std::string& revision = require_string(
          require_member(capability, "capability_definition_revision",
                         "component capability"),
          "component capability.capability_definition_revision");
      if (!selected_keys.emplace(id, revision).second) {
        continue;
      }
      const auto* qualified = find_qualified_capability(
          retained_capabilities, id, revision);
      if (qualified == nullptr) {
        fail("component type selects a missing or ambiguous retained "
             "capability definition");
      }
      selected.push_back(*qualified);
    }
  }
  return selected;
}

[[nodiscard]] QualifiedComponentType load_component_type(
    const core::RetainedBundleDTO& bundle,
    const core::ControllableComponentDTO& component,
    std::span<const QualifiedCapabilityDefinition> retained_capabilities,
    const ICatalogArtifactSource& store) {
  const auto& record =
      require_catalog_record<core::ComponentTypeCatalogRecordRefDTO>(
          bundle, component.component_type_id.value(),
          component.component_type_revision.value(), "component type");
  const std::string bytes = read_catalog_record(record, store, "component type");
  const auto dependencies = select_component_capability_dependencies(
      bytes, retained_capabilities);
  return resolve_and_qualify_component_type(record,
                                            record.content_addressed_store_key,
                                            bytes, dependencies);
}

template <typename Value, typename Key>
void sort_unique(std::vector<Value>& values, Key key,
                 std::string_view label) {
  std::sort(values.begin(), values.end(), [&](const auto& left,
                                               const auto& right) {
    return key(left) < key(right);
  });
  if (std::adjacent_find(values.begin(), values.end(),
                         [&](const auto& left, const auto& right) {
                           return key(left) == key(right);
                         }) != values.end()) {
    fail(std::string(label) + ": duplicate role assignment");
  }
}

void normalize_role_assignment(
    const core::ProgramBindingContractDTO& contract,
    core::ProgramRoleAssignmentDTO& assignment) {
  if (assignment.component_roles.size() != contract.component_roles.size() ||
      assignment.target_roles.size() != contract.target_roles.size() ||
      assignment.component_roles.size() > core::kMaxProgramContractRoles ||
      assignment.target_roles.size() > core::kMaxProgramContractRoles) {
    fail("role assignment is not complete for the Program contract");
  }
  sort_unique(assignment.component_roles,
              [](const auto& value) { return value.component_role_id; },
              "component role assignments");
  sort_unique(assignment.target_roles,
              [](const auto& value) { return value.target_role_id; },
              "target role assignments");
  for (const auto& requirement : contract.component_roles) {
    if (std::none_of(assignment.component_roles.begin(),
                     assignment.component_roles.end(),
                     [&](const auto& value) {
                       return value.component_role_id ==
                              requirement.component_role_id;
                     })) {
      fail("role assignment omits a declared component role");
    }
  }
  for (const auto& requirement : contract.target_roles) {
    if (std::none_of(assignment.target_roles.begin(),
                     assignment.target_roles.end(), [&](const auto& value) {
                       return value.target_role_id == requirement.target_role_id;
                     })) {
      fail("role assignment omits a declared target role");
    }
  }
}

[[nodiscard]] core::ContentDigest role_assignment_digest(
    const core::ProgramRoleAssignmentDTO& assignment) {
  CanonicalValue::Array components;
  components.reserve(assignment.component_roles.size());
  for (const auto& value : assignment.component_roles) {
    components.emplace_back(CanonicalValue::Object{
        {"component_role_id",
         CanonicalValue(value.component_role_id.value())},
        {"controllable_component_id",
         CanonicalValue(value.controllable_component_id.value())},
    });
  }
  CanonicalValue::Array targets;
  targets.reserve(assignment.target_roles.size());
  for (const auto& value : assignment.target_roles) {
    targets.emplace_back(CanonicalValue::Object{
        {"target_role_id", CanonicalValue(value.target_role_id.value())},
        {"command_target_id", CanonicalValue(value.command_target_id.value())},
    });
  }
  const std::string projection = canonical_json(CanonicalValue(
      CanonicalValue::Object{{"component_roles",
                              CanonicalValue(std::move(components))},
                             {"target_roles",
                              CanonicalValue(std::move(targets))}}));
  std::string preimage{"manyforge.program-role-assignment.v1"};
  preimage.push_back('\0');
  preimage.append(projection);
  return core::ContentDigest(sha256_digest(preimage));
}

[[nodiscard]] const core::ControllableComponentDTO& require_component(
    const core::CellGraphDTO& graph,
    const core::ControllableComponentId& id) {
  const auto* component = find_exactly_one<core::ControllableComponentDTO>(
      graph.controllable_components,
      [&](const auto& value) { return value.controllable_component_id == id; });
  if (component == nullptr) {
    fail("component role selects a missing or ambiguous retained component");
  }
  return *component;
}

[[nodiscard]] const core::ModelInstanceDTO& require_model(
    const core::CellGraphDTO& graph, const core::ModelInstanceId& id) {
  const auto* model = find_exactly_one<core::ModelInstanceDTO>(
      graph.model_instances,
      [&](const auto& value) { return value.model_instance_id == id; });
  if (model == nullptr) {
    fail("component role selects a missing or ambiguous retained model");
  }
  return *model;
}

[[nodiscard]] const core::ProgramComponentRoleAssignmentDTO&
require_component_assignment(
    const core::ProgramRoleAssignmentDTO& assignment,
    const core::ComponentRoleId& role) {
  const auto found = std::lower_bound(
      assignment.component_roles.begin(), assignment.component_roles.end(),
      role, [](const auto& value, const core::ComponentRoleId& expected) {
        return value.component_role_id < expected;
      });
  if (found == assignment.component_roles.end() ||
      found->component_role_id != role) {
    fail("missing component-role assignment");
  }
  return *found;
}

[[nodiscard]] const core::ProgramTargetRoleAssignmentDTO&
require_target_assignment(const core::ProgramRoleAssignmentDTO& assignment,
                          const core::TargetRoleId& role) {
  const auto found = std::lower_bound(
      assignment.target_roles.begin(), assignment.target_roles.end(), role,
      [](const auto& value, const core::TargetRoleId& expected) {
        return value.target_role_id < expected;
      });
  if (found == assignment.target_roles.end() || found->target_role_id != role) {
    fail("missing target-role assignment");
  }
  return *found;
}

[[nodiscard]] bool contains(std::span<const std::string> values,
                            std::string_view expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

template <typename Value, typename Id>
[[nodiscard]] bool contains_id(std::span<const Value> values,
                               const Id& expected) {
  return std::any_of(values.begin(), values.end(), [&](const auto& value) {
    if constexpr (std::is_same_v<Value, core::ComponentTypeModelRoleDTO>) {
      return value.model_role_id == expected;
    } else if constexpr (std::is_same_v<
                             Value,
                             core::ComponentTypeAttachmentInterfaceDTO>) {
      return value.attachment_interface_id == expected;
    } else {
      return false;
    }
  });
}

[[nodiscard]] core::ProgramComponentRoleBindingDTO resolve_component_role(
    const core::ProgramComponentRoleRequirementDTO& requirement,
    const core::ProgramComponentRoleAssignmentDTO& assignment,
    const core::CellGraphDTO& graph,
    const QualifiedComponentType& component_type) {
  const auto& component = require_component(
      graph, assignment.controllable_component_id);
  const auto& type = component_type.value();
  for (const auto& tag : requirement.required_component_type_tags) {
    if (!contains(type.tags, tag)) {
      fail("component role requirement is not satisfied by component tags");
    }
  }
  for (const auto& role : requirement.required_model_roles) {
    if (!contains_id<core::ComponentTypeModelRoleDTO>(type.model_roles, role)) {
      fail("component role requires an unavailable model role");
    }
  }
  for (const auto& interface_id :
       requirement.required_attachment_interfaces) {
    if (!contains_id<core::ComponentTypeAttachmentInterfaceDTO>(
            type.attachment_interfaces, interface_id)) {
      fail("component role requires an unavailable attachment interface");
    }
  }

  std::vector<core::ProgramModelRoleBindingDTO> models;
  models.reserve(component.model_role_bindings.size());
  for (const auto& role : component.model_role_bindings) {
    if (!contains_id<core::ComponentTypeModelRoleDTO>(type.model_roles,
                                                       role.model_role_id)) {
      fail("retained component contains a model role absent from its type");
    }
    const auto& model = require_model(graph, role.model_instance_id);
    models.push_back(core::ProgramModelRoleBindingDTO{
        role.model_role_id, role.model_instance_id, model.model_revision});
  }
  for (const auto& required : requirement.required_model_roles) {
    if (std::none_of(models.begin(), models.end(), [&](const auto& value) {
          return value.model_role_id == required;
        })) {
      fail("component role has no retained instance for a required model role");
    }
  }
  return core::ProgramComponentRoleBindingDTO{
      requirement.component_role_id, component.controllable_component_id,
      component.component_type_id, component.component_type_revision,
      std::move(models)};
}

[[nodiscard]] const core::ComponentTypeCommandTargetDTO& require_target_type(
    const QualifiedComponentType& component_type,
    const core::CommandTargetId& target_id) {
  const auto* target =
      find_exactly_one<core::ComponentTypeCommandTargetDTO>(
          component_type.value().command_targets, [&](const auto& value) {
            return value.command_target_id == target_id;
          });
  if (target == nullptr) {
    fail("target role selects a missing or ambiguous component target");
  }
  return *target;
}

[[nodiscard]] core::CoordinateKind coordinate_kind(
    UrdfCoordinateKind value) {
  if (value == UrdfCoordinateKind::kAngular) {
    return core::CoordinateKind::kAngular;
  }
  if (value == UrdfCoordinateKind::kLinear) {
    return core::CoordinateKind::kLinear;
  }
  fail("command target contains a non-scalar coordinate kind");
}

struct ResolvedResourceRole final {
  core::ProgramResourceRoleBindingDTO binding;
  std::vector<std::optional<core::CoordinateKind>> coordinate_kinds;

  friend bool operator==(const ResolvedResourceRole&,
                         const ResolvedResourceRole&) = default;
};

[[nodiscard]] ResolvedResourceRole resolve_resource_role(
    const VerifiedRetainedModelMap& model_map,
    const core::ComponentTypeCommandTargetDTO& target_type,
    const core::ControllableComponentId& component_id,
    const core::CommandTargetId& target_id,
    const core::ResourceRoleId& resource_role_id) {
  const auto declared_role = std::find_if(
      target_type.resource_roles.begin(), target_type.resource_roles.end(),
      [&](const auto& value) {
        return core::resource_role_id(value) == resource_role_id;
      });
  if (declared_role == target_type.resource_roles.end()) {
    fail("target resource role is absent from the component type");
  }
  const auto* local_role =
      std::get_if<core::ComponentTypeLocalResourceRoleDTO>(&*declared_role);
  std::optional<ResolvedResourceRole> result;
  for (const auto& coordinate_map : model_map.control_bindings()) {
    if (coordinate_map.controllable_component_id != component_id ||
        coordinate_map.command_target_id != target_id) {
      continue;
    }
    ResolvedResourceRole candidate{
        core::ProgramResourceRoleBindingDTO{resource_role_id, {}, {}}, {}};
    for (const auto& row : coordinate_map.resources) {
      if (row.resource_role_id != resource_role_id) {
        continue;
      }
      candidate.binding.resource_ids.push_back(row.resource_id);
      candidate.binding.compiled_resource_names.push_back(
          row.coordinates.empty()
              ? row.resource_id.value()
              : row.coordinates.front().compiled_joint_name);
      if (row.coordinates.empty()) {
        if (local_role == nullptr ||
            row.component_local_coordinate_kind !=
                local_role->coordinate_kind) {
          fail("component-local resource coordinate kind differs from its "
               "component type");
        }
        candidate.coordinate_kinds.push_back(
            row.component_local_coordinate_kind);
      } else {
        if (local_role != nullptr ||
            row.component_local_coordinate_kind.has_value()) {
          fail("model-backed resource carries component-local coordinate "
               "semantics");
        }
        candidate.coordinate_kinds.push_back(
            coordinate_kind(row.coordinates.front().coordinate_kind));
      }
    }
    if (candidate.binding.resource_ids.empty()) {
      continue;
    }
    if (result.has_value() && *result != candidate) {
      fail("target resource role has conflicting retained control mappings");
    }
    result = std::move(candidate);
  }
  if (!result.has_value()) {
    fail("target resource role has no retained coordinate mapping");
  }
  return std::move(*result);
}

[[nodiscard]] const CompositionTargetFrameResolution& require_frame(
    const CompositionTargetResolution& target,
    const core::FrameRoleId& role) {
  const auto* frame = find_exactly_one<CompositionTargetFrameResolution>(
      target.frames,
      [&](const auto& value) { return value.frame_role_id == role; });
  if (frame == nullptr) {
    fail("target role requires a missing or ambiguous frame role");
  }
  return *frame;
}

[[nodiscard]] const CompositionTargetCollisionResolution& require_collision(
    const CompositionTargetResolution& target,
    const core::CollisionBodyRoleId& role) {
  const auto* body = find_exactly_one<CompositionTargetCollisionResolution>(
      target.collision_bodies, [&](const auto& value) {
        return value.collision_body_role_id == role;
      });
  if (body == nullptr) {
    fail("target role requires a missing or ambiguous collision-body role");
  }
  return *body;
}

[[nodiscard]] const core::ControlBindingDTO* find_control_binding(
    const core::CellGraphDTO& graph,
    const core::ControlBindingId& binding_id) {
  return find_exactly_one<core::ControlBindingDTO>(
      graph.control_bindings, [&](const auto& value) {
        return value.control_binding_id == binding_id;
      });
}

[[nodiscard]] bool provider_target_matches(
    const core::CapabilityProviderBindingDTO& provider,
    const core::CellGraphDTO& graph,
    const core::ControllableComponentDTO& component,
    const core::CommandTargetId& target_id) {
  return std::visit(
      [&](const auto& target) {
        using Target = std::remove_cvref_t<decltype(target)>;
        if constexpr (std::is_same_v<
                          Target, core::CellGlobalCapabilityProviderTargetDTO>) {
          return provider.scope == core::CapabilityScope::kCellGlobal &&
                 target.cell_id == graph.cell_id;
        } else if constexpr (std::is_same_v<
                                 Target,
                                 core::TargetBindingCapabilityProviderTargetDTO>) {
          const auto* control =
              find_control_binding(graph, target.control_binding_id);
          return provider.scope == core::CapabilityScope::kTargetBinding &&
                 control != nullptr &&
                 target.controllable_component_id ==
                     component.controllable_component_id &&
                 target.component_type_revision ==
                     component.component_type_revision &&
                 target.command_target_id == target_id &&
                 target.control_binding_revision ==
                     control->control_binding_revision &&
                 control->controllable_component_id ==
                     component.controllable_component_id &&
                 control->command_target_id == target_id;
        } else {
          const auto* domain = find_exactly_one<core::ControlDomainDTO>(
              graph.control_domains, [&](const auto& value) {
                return value.control_domain_id == target.control_domain_id;
              });
          const bool target_uses_domain = std::any_of(
              graph.control_bindings.begin(), graph.control_bindings.end(),
              [&](const auto& binding) {
                return binding.controllable_component_id ==
                           component.controllable_component_id &&
                       binding.command_target_id == target_id &&
                       binding.control_domain_id == target.control_domain_id;
              });
          return provider.scope == core::CapabilityScope::kControlDomain &&
                 domain != nullptr && target_uses_domain &&
                 target.control_domain_revision ==
                     domain->control_domain_revision &&
                 target.controller_topology_id ==
                     domain->controller_topology_id &&
                 target.topology_revision == domain->topology_revision;
        }
      },
      provider.target);
}

void validate_command_schema_target(
    const QualifiedCommandSchema& schema,
    const core::ComponentTypeCommandTargetDTO& target_type,
    const VerifiedRetainedModelMap& model_map,
    const core::ControllableComponentId& component_id) {
  const auto* contract = std::get_if<core::GripperPositionCommandValueContractDTO>(
      &schema.value().command_value);
  if (contract == nullptr) {
    fail("command schema uses an unsupported command-value contract");
  }

  std::vector<core::ResourceRoleId> selected_roles;
  if (schema.value().resource_role_selectors.empty()) {
    selected_roles.reserve(target_type.resource_roles.size());
    for (const auto& role : target_type.resource_roles) {
      selected_roles.push_back(core::resource_role_id(role));
    }
  } else {
    selected_roles = schema.value().resource_role_selectors;
  }
  if (selected_roles.empty()) {
    fail("command schema selected no command-target resource roles");
  }

  std::vector<core::CoordinateKind> coordinate_kinds;
  for (const auto& role : selected_roles) {
    const auto declared_role = std::find_if(
        target_type.resource_roles.begin(), target_type.resource_roles.end(),
        [&](const auto& value) {
          return core::resource_role_id(value) == role;
        });
    if (declared_role == target_type.resource_roles.end()) {
      fail("command schema selects a role absent from the command target");
    }
    const bool component_local =
        std::holds_alternative<core::ComponentTypeLocalResourceRoleDTO>(
            *declared_role);
    const auto resolved = resolve_resource_role(
        model_map, target_type, component_id,
        target_type.command_target_id, role);
    for (const auto& kind : resolved.coordinate_kinds) {
      if (!kind.has_value()) {
        fail(component_local
                 ? "scalar command schema requires an explicit "
                   "component-local coordinate kind"
                 : "scalar command schema has no exact model coordinate "
                   "kind");
      }
      coordinate_kinds.push_back(*kind);
    }
  }
  if (coordinate_kinds.size() < contract->minimum_resource_count ||
      coordinate_kinds.size() > contract->maximum_resource_count) {
    fail("command target resource count violates its command schema");
  }
  if (!std::all_of(coordinate_kinds.begin(), coordinate_kinds.end(),
                   [&](const auto kind) {
                     return kind == coordinate_kinds.front();
                   }) ||
      std::find(contract->accepted_position_kinds.begin(),
                contract->accepted_position_kinds.end(),
                coordinate_kinds.front()) ==
          contract->accepted_position_kinds.end()) {
    fail("command target coordinate kinds violate its command schema");
  }
}

[[nodiscard]] std::optional<core::ProgramCapabilityBindingDTO>
resolve_capability(
    const core::ProgramCapabilityRequirementDTO& requirement,
    const core::ComponentTypeCommandTargetDTO& target_type,
    const core::ControllableComponentDTO& component,
    const core::CellGraphDTO& graph,
    const core::RetainedBundleDTO& bundle,
    const QualifiedCatalogClosure& catalog,
    const VerifiedRetainedModelMap& model_map) {
  const auto* qualified = find_qualified_capability(
      catalog.capabilities, requirement.capability_id.value(),
      requirement.capability_definition_revision.value());
  if (qualified == nullptr) {
    fail("Program capability is absent or ambiguous in the retained catalog");
  }
  const auto& definition = qualified->value();
  if (definition.scope == core::CapabilityScope::kTargetBinding) {
    const bool target_declares = std::any_of(
        target_type.capabilities.begin(), target_type.capabilities.end(),
        [&](const auto& value) {
          return value.capability_id == requirement.capability_id &&
                 value.capability_definition_revision ==
                     requirement.capability_definition_revision;
        });
    if (!target_declares) {
      if (requirement.disposition ==
          core::ProgramCapabilityDisposition::kOptional) {
        return std::nullopt;
      }
      fail("required target capability is absent from the component type");
    }
  }

  std::optional<core::ProgramControlBindingRefDTO> control_ref;
  std::optional<core::ProgramCapabilityProviderBindingRefDTO> provider_ref;
  if (definition.provider_binding_kind ==
      core::CapabilityProviderBindingKind::kControlBinding) {
    const core::ControlBindingDTO* match = nullptr;
    for (const auto& binding : graph.control_bindings) {
      if (binding.controllable_component_id !=
              component.controllable_component_id ||
          binding.command_target_id != target_type.command_target_id ||
          binding.capability_id != requirement.capability_id ||
          binding.capability_definition_revision !=
              requirement.capability_definition_revision) {
        continue;
      }
      if (match != nullptr) {
        fail("Program capability has ambiguous retained control bindings");
      }
      match = &binding;
    }
    if (match != nullptr) {
      control_ref = core::ProgramControlBindingRefDTO{
          match->control_binding_id, match->control_binding_revision};
    }
  } else {
    const core::CapabilityProviderBindingDTO* match = nullptr;
    for (const auto& provider : bundle.capability_provider_bindings) {
      if (provider.capability_id != requirement.capability_id ||
          provider.capability_definition_revision !=
              requirement.capability_definition_revision ||
          !provider_target_matches(provider, graph, component,
                                   target_type.command_target_id)) {
        continue;
      }
      if (match != nullptr) {
        fail("Program capability has ambiguous retained provider bindings");
      }
      match = &provider;
    }
    if (match != nullptr) {
      provider_ref = core::ProgramCapabilityProviderBindingRefDTO{
          match->capability_provider_binding_id,
          match->capability_provider_binding_revision};
    }
  }
  if (!control_ref.has_value() && !provider_ref.has_value()) {
    if (requirement.disposition ==
        core::ProgramCapabilityDisposition::kOptional) {
      return std::nullopt;
    }
    fail("required Program capability has no exact retained provider");
  }

  const QualifiedCommandSchema* selected_schema = nullptr;
  for (const auto& schema : catalog.command_schemas) {
    const bool compatible = std::any_of(
        schema.value().compatible_capabilities.begin(),
        schema.value().compatible_capabilities.end(), [&](const auto& value) {
          return value.capability_id == requirement.capability_id &&
                 value.capability_definition_revision ==
                     requirement.capability_definition_revision;
        });
    if (!compatible) {
      continue;
    }
    if (selected_schema != nullptr) {
      fail("Program capability has ambiguous retained command schemas");
    }
    selected_schema = &schema;
  }
  const bool requires_current_command_schema =
      requirement.capability_id.value() == "tool.gripper.command";
  if (requires_current_command_schema != (selected_schema != nullptr)) {
    fail("Program capability command-schema closure is incomplete or "
         "incompatible");
  }

  std::optional<core::ProgramCommandSchemaRefDTO> command_schema;
  if (selected_schema != nullptr) {
    validate_command_schema_target(*selected_schema, target_type, model_map,
                                   component.controllable_component_id);
    command_schema = core::ProgramCommandSchemaRefDTO{
        selected_schema->value().command_schema_id,
        selected_schema->value().command_schema_revision};
  }
  return core::ProgramCapabilityBindingDTO{
      requirement.capability_id,
      requirement.capability_definition_revision,
      std::move(control_ref), std::move(provider_ref),
      std::move(command_schema)};
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
    const core::ProgramComponentRoleBindingDTO& component,
    std::vector<core::RetainedSourceRecordDTO>& output) {
  add_resolved_source_record(
      bundle, core::CatalogRecordKind::kComponentType,
      component.component_type_id.value(),
      component.component_type_revision.value(), output);
  for (const auto& model_binding : component.model_role_bindings) {
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
    const std::vector<core::ProgramComponentRoleBindingDTO>& components,
    const std::vector<core::ProgramTargetRoleBindingDTO>& targets) {
  std::vector<core::ProgramContactGraspBindingDTO> resolved;
  resolved.reserve(contract.contact_grasp_templates.size());
  for (const auto& policy : contract.contact_grasp_templates) {
    std::vector<core::ProgramContactGraspBodyBindingDTO> bodies;
    bodies.reserve(policy.allowed_body_selectors.size());
    for (const auto& selector : policy.allowed_body_selectors) {
      const auto component = std::find_if(
          components.begin(), components.end(), [&](const auto& candidate) {
            return candidate.component_role_id == selector.component_role_id;
          });
      if (component == components.end()) {
        fail("contact-grasp selector has no resolved component role");
      }
      const core::ProgramTargetRoleBindingDTO* selected_target = nullptr;
      const core::ProgramCollisionBodyRoleBindingDTO* selected_body = nullptr;
      for (const auto& target : targets) {
        if (target.component_role_id != selector.component_role_id) {
          continue;
        }
        const auto collision = std::find_if(
            target.collision_body_role_bindings.begin(),
            target.collision_body_role_bindings.end(),
            [&](const auto& candidate) {
              return candidate.collision_body_role_id ==
                     selector.collision_body_role_id;
            });
        if (collision == target.collision_body_role_bindings.end()) {
          continue;
        }
        if (selector.model_role_id.has_value()) {
          const auto model = std::find_if(
              component->model_role_bindings.begin(),
              component->model_role_bindings.end(), [&](const auto& candidate) {
                return candidate.model_role_id == *selector.model_role_id;
              });
          if (model == component->model_role_bindings.end() ||
              model->model_instance_id != collision->model_instance_id ||
              model->model_revision != collision->model_revision) {
            continue;
          }
        }
        if (selected_target != nullptr) {
          fail("contact-grasp selector resolves to more than one Program "
               "target");
        }
        selected_target = &target;
        selected_body = &*collision;
      }
      if (selected_target == nullptr || selected_body == nullptr) {
        fail("contact-grasp selector has no exact Program target/body "
             "resolution");
      }
      std::vector<core::CollisionBodyId> body_ids;
      body_ids.reserve(selected_body->compiled_collision_body_names.size());
      for (std::size_t collision_index = 0U;
           collision_index <
           selected_body->compiled_collision_body_names.size();
           ++collision_index) {
        body_ids.push_back(core::make_component_collision_body_id(
            selected_target->controllable_component_id,
            selected_body->model_instance_id,
            selected_body->collision_body_role_id, collision_index));
      }
      bodies.push_back(core::ProgramContactGraspBodyBindingDTO{
          selector.component_role_id,
          selected_target->target_role_id,
          selected_target->controllable_component_id,
          selected_target->command_target_id,
          selector.model_role_id,
          selector.collision_body_role_id,
          selected_body->model_instance_id,
          selected_body->model_revision,
          std::move(body_ids)});
    }
    resolved.push_back(core::ProgramContactGraspBindingDTO{
        policy.contact_policy_id, policy.contact_policy_revision,
        policy.runtime_object_id, std::move(bodies), policy.max_penetration_m,
        policy.terminal_window_start_fraction});
  }
  return resolved;
}

}  // namespace

namespace {

[[nodiscard]] std::shared_ptr<const ProgramBinding>
compile_program_binding_contract(
    const core::ProgramBindingContractDTO& program_contract,
    core::ProgramRoleAssignmentDTO role_assignment,
    const RetainedBundle& retained_bundle,
    const VerifiedRetainedModelMap& retained_model_map,
    const ICatalogArtifactSource& catalog_artifact_store) {
  try {
    if (!verify_retained_bundle_identity(retained_bundle) ||
        retained_model_map.query().retained_bundle_id !=
            retained_bundle.retained_bundle_id()) {
      fail("binding compiler inputs do not share one verified retained bundle");
    }
    const auto& bundle = retained_bundle.value();
    if (program_contract.node_environment != bundle.catalogs.node_environment) {
      fail("Program contract selects another retained node environment");
    }
    normalize_role_assignment(program_contract, role_assignment);

    const QualifiedCatalogClosure catalog = load_qualified_catalog_closure(
        bundle, catalog_artifact_store);

    // Strictly compiling the package-owned catalog closure proves this
    // compiler understands the selected contracts. Producer build/source
    // identities are retained provenance, not compatibility requirements.
    const auto current = detail::BuildQualifiedRetentionAuthority::qualify();

    std::map<std::pair<std::string, std::string>, QualifiedComponentType>
        component_types;
    std::vector<core::ProgramComponentRoleBindingDTO> component_bindings;
    component_bindings.reserve(program_contract.component_roles.size());
    for (const auto& requirement : program_contract.component_roles) {
      const auto& assignment =
          require_component_assignment(role_assignment,
                                       requirement.component_role_id);
      const auto& component = require_component(
          bundle.cell_graph, assignment.controllable_component_id);
      const auto key = std::make_pair(component.component_type_id.value(),
                                      component.component_type_revision.value());
      auto type = component_types.find(key);
      if (type == component_types.end()) {
        type = component_types
                   .emplace(key, load_component_type(
                                     bundle, component, catalog.capabilities,
                                     catalog_artifact_store))
                   .first;
      }
      component_bindings.push_back(resolve_component_role(
          requirement, assignment, bundle.cell_graph, type->second));
    }

    std::vector<core::ProgramTargetRoleBindingDTO> target_bindings;
    target_bindings.reserve(program_contract.target_roles.size());
    for (const auto& requirement : program_contract.target_roles) {
      const auto& component_assignment = require_component_assignment(
          role_assignment, requirement.component_role_id);
      const auto& target_assignment =
          require_target_assignment(role_assignment, requirement.target_role_id);
      const auto& component = require_component(
          bundle.cell_graph, component_assignment.controllable_component_id);
      const auto type = component_types.find(std::make_pair(
          component.component_type_id.value(),
          component.component_type_revision.value()));
      if (type == component_types.end()) {
        fail("target role has no qualified component-role type");
      }
      const auto& target_type =
          require_target_type(type->second, target_assignment.command_target_id);
      const auto* target = retained_model_map.find_target(
          component.controllable_component_id,
          target_assignment.command_target_id);
      if (target == nullptr) {
        fail("target role has no exact retained model-map target");
      }

      const auto component_binding = std::find_if(
          component_bindings.begin(), component_bindings.end(),
          [&](const auto& value) {
            return value.component_role_id == requirement.component_role_id;
          });
      if (component_binding == component_bindings.end()) {
        fail("target role has no resolved component role");
      }
      for (const auto& role : requirement.required_model_roles) {
        if (std::none_of(component_binding->model_role_bindings.begin(),
                         component_binding->model_role_bindings.end(),
                         [&](const auto& value) {
                           return value.model_role_id == role;
                         })) {
          fail("target role requires an unavailable component model role");
        }
      }

      std::vector<core::ProgramFrameRoleBindingDTO> frames;
      for (const auto& role : requirement.required_frame_roles) {
        if (std::none_of(target_type.frame_roles.begin(),
                         target_type.frame_roles.end(), [&](const auto& value) {
                           return value.frame_role_id == role;
                         })) {
          fail("target role requires a frame absent from its component type");
        }
        const auto& frame = require_frame(*target, role);
        const auto* resolved =
            std::get_if<CompositionComponentModelFrame>(&frame.frame);
        if (resolved == nullptr ||
            resolved->controllable_component_id !=
                component.controllable_component_id) {
          fail("target frame does not resolve to the selected component");
        }
        frames.push_back(core::ProgramFrameRoleBindingDTO{
            role, resolved->model_instance_id, resolved->model_revision,
            resolved->compiled_frame_name});
      }

      std::vector<core::ProgramResourceRoleBindingDTO> resources;
      for (const auto& role : requirement.required_resource_roles) {
        if (std::none_of(target_type.resource_roles.begin(),
                         target_type.resource_roles.end(), [&](const auto& value) {
                           return core::resource_role_id(value) == role;
                         })) {
          fail("target role requires a resource absent from its component type");
        }
        resources.push_back(resolve_resource_role(
                                retained_model_map, target_type,
                                component.controllable_component_id,
                                target_assignment.command_target_id, role)
                                .binding);
      }

      std::vector<core::ProgramCollisionBodyRoleBindingDTO> collision_bodies;
      for (const auto& role : requirement.required_collision_body_roles) {
        if (std::none_of(target_type.collision_body_roles.begin(),
                         target_type.collision_body_roles.end(),
                         [&](const auto& value) {
                           return value.collision_body_role_id == role;
                         })) {
          fail("target role requires a collision body absent from its "
               "component type");
        }
        const auto& collision = require_collision(*target, role);
        const auto& model =
            require_model(bundle.cell_graph, collision.model_instance_id);
        collision_bodies.push_back(
            core::ProgramCollisionBodyRoleBindingDTO{
                role, collision.model_instance_id, model.model_revision,
                collision.compiled_link_names});
      }

      std::vector<core::ProgramCapabilityBindingDTO> capabilities;
      for (const auto& capability : requirement.required_capabilities) {
        auto resolved = resolve_capability(
            capability, target_type, component, bundle.cell_graph, bundle,
            catalog, retained_model_map);
        if (resolved.has_value()) {
          capabilities.push_back(std::move(*resolved));
        }
      }
      target_bindings.push_back(core::ProgramTargetRoleBindingDTO{
          requirement.target_role_id, requirement.component_role_id,
          component.controllable_component_id,
          target_assignment.command_target_id, std::move(frames),
          std::move(resources), std::move(collision_bodies),
          std::move(capabilities)});
    }

    std::vector<core::RetainedSourceRecordDTO> resolved_records;
    for (const auto& component : component_bindings) {
      add_component_provenance(bundle, component, resolved_records);
    }
    for (const auto& target : target_bindings) {
      for (const auto& capability : target.capability_bindings) {
        add_capability_provenance(bundle, capability, resolved_records);
      }
    }

    core::ProgramBindingDTO binding_value{
            std::string(core::kProgramBindingApiVersion),
            core::ProgramBindingId(std::string(kZeroDigest)),
            program_contract.program_id,
            program_contract.program_revision,
            retained_bundle.retained_bundle_id(),
            bundle.cell_graph.graph_revision,
            bundle.catalogs.node_environment,
            std::move(component_bindings), std::move(target_bindings),
            core::ProgramBindingProvenanceDTO{
                current.compiler_contract_digest(),
                current.compiler_build_digest(),
                role_assignment_digest(role_assignment),
                bundle.provenance.source_records,
                std::move(resolved_records)}};
    binding_value.contact_grasp_bindings = resolve_contact_grasp_bindings(
        program_contract, binding_value.component_role_bindings,
        binding_value.target_role_bindings);
    binding_value.node_policies = program_contract.node_policies;
    auto sealed = detail::ProgramBindingConstructionAuthority::seal(
        std::move(binding_value));
    return reload_verified_program_binding(sealed->canonical_bytes(),
                                           retained_bundle,
                                           retained_model_map);
  } catch (const ProgramBindingCompilerError&) {
    throw;
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

[[nodiscard]] core::ProgramBindingContractDTO
make_direct_control_motion_contract(
    const core::RetainedNodeEnvironmentDTO& node_environment,
    const core::ComponentTypeCommandTargetDTO& target) {
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
  for (const auto required_role : {kDirectControlBaseFrameRoleId,
                                   kDirectControlTcpFrameRoleId}) {
    if (std::none_of(target.frame_roles.begin(), target.frame_roles.end(),
                     [&](const auto& role) {
                       return role.frame_role_id.value() == required_role;
                     })) {
      fail("direct-control motion target omits its logical " +
           std::string(required_role) + " frame role");
    }
  }

  const auto model_roles =
      sorted_ids<core::ModelRoleId>(model_role_names);
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
  const auto frame_roles =
      sorted_ids<core::FrameRoleId>(frame_role_names);
  const auto resource_roles =
      sorted_ids<core::ResourceRoleId>(resource_role_names);
  const auto collision_roles =
      sorted_ids<core::CollisionBodyRoleId>(collision_role_names);

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
       CanonicalValue("manyforge.direct_control_motion_program.v2")},
      {"component_role_id",
       CanonicalValue(std::string(kDirectControlComponentRoleId))},
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
      {"target_role",
       CanonicalValue(CanonicalValue::Object{
           {"capabilities",
            CanonicalValue(std::move(capability_projection))},
           {"collision_body_roles", strong_id_array(collision_roles)},
           {"frame_roles", strong_id_array(frame_roles)},
           {"resource_roles", strong_id_array(resource_roles)},
           {"target_role_id",
            CanonicalValue(std::string(kDirectControlTargetRoleId))},
       })},
  });
  std::string revision_preimage{
      "manyforge.direct-control-motion-program-revision.v2"};
  revision_preimage.push_back('\0');
  revision_preimage.append(canonical_json(projection));

  return core::ProgramBindingContractDTO{
      core::ProgramId(std::string(kDirectControlMotionProgramId)),
      core::ProgramRevision(sha256_digest(revision_preimage)),
      node_environment,
      {core::ProgramComponentRoleRequirementDTO{
          core::ComponentRoleId(
              std::string(kDirectControlComponentRoleId)),
          {}, model_roles, {}}},
      {core::ProgramTargetRoleRequirementDTO{
          core::TargetRoleId(std::string(kDirectControlTargetRoleId)),
          core::ComponentRoleId(
              std::string(kDirectControlComponentRoleId)),
          std::move(capabilities), frame_roles, resource_roles,
          collision_roles, model_roles}},
      {}};
}

[[nodiscard]] core::ProgramBindingContractDTO
make_direct_control_gripper_contract(
    const core::RetainedNodeEnvironmentDTO& node_environment,
    const core::ComponentTypeCommandTargetDTO& target) {
  const auto* command_capability =
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
       CanonicalValue("manyforge.direct_control_gripper_program.v2")},
      {"component_role_id",
       CanonicalValue(std::string(kDirectControlComponentRoleId))},
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
      {"target_role",
       CanonicalValue(CanonicalValue::Object{
           {"capabilities",
            CanonicalValue(std::move(capability_projection))},
           {"collision_body_roles", strong_id_array(collision_roles)},
           {"frame_roles", strong_id_array(frame_roles)},
           {"resource_roles", strong_id_array(resource_roles)},
           {"target_role_id",
            CanonicalValue(std::string(kDirectControlTargetRoleId))},
       })},
  });
  std::string revision_preimage{
      "manyforge.direct-control-gripper-program-revision.v2"};
  revision_preimage.push_back('\0');
  revision_preimage.append(canonical_json(projection));

  return core::ProgramBindingContractDTO{
      core::ProgramId(std::string(kDirectControlGripperProgramId)),
      core::ProgramRevision(sha256_digest(revision_preimage)),
      node_environment,
      {core::ProgramComponentRoleRequirementDTO{
          core::ComponentRoleId(std::string(kDirectControlComponentRoleId)),
          {}, model_roles, {}}},
      {core::ProgramTargetRoleRequirementDTO{
          core::TargetRoleId(std::string(kDirectControlTargetRoleId)),
          core::ComponentRoleId(std::string(kDirectControlComponentRoleId)),
          std::move(capabilities), frame_roles, resource_roles,
          collision_roles, model_roles}},
      {}};
}

}  // namespace

std::shared_ptr<const ProgramBinding> compile_program_binding(
    const ProgramBindingContract& program_contract,
    core::ProgramRoleAssignmentDTO role_assignment,
    const RetainedBundle& retained_bundle,
    const VerifiedRetainedModelMap& retained_model_map,
    const ICatalogArtifactSource& catalog_artifact_store) {
  return compile_program_binding_contract(
      program_contract.value(), std::move(role_assignment), retained_bundle,
      retained_model_map, catalog_artifact_store);
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
    auto contract = make_direct_control_motion_contract(
        bundle.catalogs.node_environment, target);
    core::ProgramRoleAssignmentDTO assignment{
        {core::ProgramComponentRoleAssignmentDTO{
            core::ComponentRoleId(
                std::string(kDirectControlComponentRoleId)),
            controllable_component_id}},
        {core::ProgramTargetRoleAssignmentDTO{
            core::TargetRoleId(std::string(kDirectControlTargetRoleId)),
            command_target_id}}};
    return compile_program_binding_contract(
        contract, std::move(assignment), retained_bundle,
        retained_model_map, catalog_artifact_store);
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
        bundle.catalogs.node_environment, target);
    core::ProgramRoleAssignmentDTO assignment{
        {core::ProgramComponentRoleAssignmentDTO{
            core::ComponentRoleId(
                std::string(kDirectControlComponentRoleId)),
            controllable_component_id}},
        {core::ProgramTargetRoleAssignmentDTO{
            core::TargetRoleId(std::string(kDirectControlTargetRoleId)),
            command_target_id}}};
    auto binding = compile_program_binding_contract(
        contract, std::move(assignment), retained_bundle,
        retained_model_map, catalog_artifact_store);
    const auto& targets = binding->value().target_role_bindings;
    if (targets.size() != 1U ||
        targets.front().resource_role_bindings.size() != 1U ||
        targets.front().resource_role_bindings.front().resource_ids.size() !=
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

DirectControlProgramBindingInventory
compile_direct_control_program_binding_inventory(
    const RetainedBundle& retained_bundle,
    const VerifiedRetainedModelMap& retained_model_map,
    const ICatalogArtifactSource& catalog_artifact_store) {
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
          auto contract = make_direct_control_motion_contract(
              bundle.catalogs.node_environment, target);
          result.motion.push_back(compile_program_binding_contract(
              contract,
              core::ProgramRoleAssignmentDTO{
                  {core::ProgramComponentRoleAssignmentDTO{
                      core::ComponentRoleId(
                          std::string(kDirectControlComponentRoleId)),
                      component.controllable_component_id}},
                  {core::ProgramTargetRoleAssignmentDTO{
                      core::TargetRoleId(
                          std::string(kDirectControlTargetRoleId)),
                      target.command_target_id}}},
              retained_bundle, retained_model_map,
              catalog_artifact_store));
        }
        if (find_target_capability(target, "tool.gripper.command") !=
            nullptr) {
          auto contract = make_direct_control_gripper_contract(
              bundle.catalogs.node_environment, target);
          result.gripper.push_back(compile_program_binding_contract(
              contract,
              core::ProgramRoleAssignmentDTO{
                  {core::ProgramComponentRoleAssignmentDTO{
                      core::ComponentRoleId(
                          std::string(kDirectControlComponentRoleId)),
                      component.controllable_component_id}},
                  {core::ProgramTargetRoleAssignmentDTO{
                      core::TargetRoleId(
                          std::string(kDirectControlTargetRoleId)),
                      target.command_target_id}}},
              retained_bundle, retained_model_map,
              catalog_artifact_store));
        }
      }
    }
    return result;
  } catch (const ProgramBindingCompilerError&) {
    throw;
  } catch (const std::exception& error) {
    fail(std::string("Direct-control Program Binding inventory compilation "
                     "failed: ") +
         error.what());
  }
}

}  // namespace manyforge::deployment
