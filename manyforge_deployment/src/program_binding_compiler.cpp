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
#include "program_io_condition_compiler_internal.hpp"
#include <manyforge_core/io_signal_descriptor.hpp>
#include <manyforge_deployment/io_signal_catalog.hpp>
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>
#include "ros2_control_execution_runtime_unit_internal.hpp"

namespace manyforge::deployment {
namespace {

constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";

[[noreturn]] void fail(std::string message) {
  throw ProgramBindingCompilerError(std::move(message));
}

/// A role does not resolve only because of a candidate-dependent fact. The
/// ordinary compiler reports it as ProgramBindingCompilerError with the same
/// message; the outcome compiler records it as a named unresolved role.
class TargetUnavailable final : public std::runtime_error {
 public:
  TargetUnavailable(const ProgramBindingUnresolvedReason reason,
                    const std::string& message)
      : std::runtime_error(message), reason_(reason) {}

  [[nodiscard]] ProgramBindingUnresolvedReason reason() const noexcept {
    return reason_;
  }

 private:
  ProgramBindingUnresolvedReason reason_;
};

[[noreturn]] void unavailable(const ProgramBindingUnresolvedReason reason,
                              const std::string& message) {
  throw TargetUnavailable(reason, message);
}

template <typename Value, typename Predicate>
[[nodiscard]] std::size_t count_matching(std::span<const Value> values,
                                         Predicate predicate) {
  return static_cast<std::size_t>(
      std::count_if(values.begin(), values.end(), predicate));
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
  const auto matches = [&](const auto& value) {
    return value.controllable_component_id == id;
  };
  const auto* component = find_exactly_one<core::ControllableComponentDTO>(
      graph.controllable_components, matches);
  if (component == nullptr) {
    if (count_matching<core::ControllableComponentDTO>(
            graph.controllable_components, matches) == 0U) {
      unavailable(ProgramBindingUnresolvedReason::kComponentMissing,
                  "component role selects a missing or ambiguous retained "
                  "component");
    }
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
      unavailable(ProgramBindingUnresolvedReason::kComponentIncompatible,
                  "component role requirement is not satisfied by component "
                  "tags");
    }
  }
  for (const auto& role : requirement.required_model_roles) {
    if (!contains_id<core::ComponentTypeModelRoleDTO>(type.model_roles, role)) {
      unavailable(ProgramBindingUnresolvedReason::kComponentIncompatible,
                  "component role requires an unavailable model role");
    }
  }
  for (const auto& interface_id :
       requirement.required_attachment_interfaces) {
    if (!contains_id<core::ComponentTypeAttachmentInterfaceDTO>(
            type.attachment_interfaces, interface_id)) {
      unavailable(ProgramBindingUnresolvedReason::kComponentIncompatible,
                  "component role requires an unavailable attachment "
                  "interface");
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
      unavailable(ProgramBindingUnresolvedReason::kComponentIncompatible,
                  "component role has no retained instance for a required "
                  "model role");
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
  const auto matches = [&](const auto& value) {
    return value.command_target_id == target_id;
  };
  const auto* target = find_exactly_one<core::ComponentTypeCommandTargetDTO>(
      component_type.value().command_targets, matches);
  if (target == nullptr) {
    if (count_matching<core::ComponentTypeCommandTargetDTO>(
            component_type.value().command_targets, matches) == 0U) {
      unavailable(ProgramBindingUnresolvedReason::kTargetMissing,
                  "target role selects a missing or ambiguous component "
                  "target");
    }
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
        } else if constexpr (std::is_same_v<Target, core::ComponentTargetCapabilityProviderTargetDTO>) {
          return target.controllable_component_id == component.controllable_component_id &&
                 target.component_type_revision == component.component_type_revision &&
                 target.command_target_id == target_id && ValidateNonCoordinateProviderTarget(graph, provider).ok();
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
          if (domain && target.provider()) {
            const auto* driver = domain->provider();
            const auto configured = ResolveRetainedIoSignalCatalog(graph);
            return driver && configured.ok() && provider.scope == core::CapabilityScope::kControlDomain &&
                target.control_domain_revision == domain->control_domain_revision &&
                driver->controllable_component_id == component.controllable_component_id &&
                driver->provider_instance_id == provider.provider_instance_id &&
                driver->provider_manifest_id == provider.provider_manifest_id &&
                driver->provider_manifest_revision == provider.provider_manifest_revision &&
                std::any_of(configured.value().begin(), configured.value().end(), [&](const auto& row) {
                  return row.runtime_unit_id == driver->provider_runtime_unit_id &&
                         row.descriptor.signal_id.controllable_component_id == component.controllable_component_id &&
                         row.command_target_id == target_id;
                });
          }
          return provider.scope == core::CapabilityScope::kControlDomain &&
                 domain != nullptr && target_uses_domain &&
                 !domain->provider() && !target.provider() &&
                 target.control_domain_revision ==
                     domain->control_domain_revision &&
                 target.controller().controller_topology_id ==
                     domain->controller().controller_topology_id &&
                 target.controller().topology_revision == domain->controller().topology_revision;
        }
      },
      provider.target);
}

void validate_command_schema_target(
    const QualifiedCommandSchema &schema,
    const core::ComponentTypeCommandTargetDTO &target_type,
    const VerifiedRetainedModelMap &model_map,
    const core::ControllableComponentId &component_id) {
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
    if (count_matching<QualifiedCapabilityDefinition>(
            catalog.capabilities, [&](const auto& capability) {
              return capability.value().capability_id ==
                     requirement.capability_id;
            }) == 0U) {
      // The candidate retains no definition at all, e.g. after its last
      // provider component was removed.
      if (requirement.disposition ==
          core::ProgramCapabilityDisposition::kOptional) {
        return std::nullopt;
      }
      unavailable(ProgramBindingUnresolvedReason::kCapabilityUnavailable,
                  "Program capability is absent or ambiguous in the retained "
                  "catalog");
    }
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
      unavailable(ProgramBindingUnresolvedReason::kCapabilityUnavailable,
                  "required target capability is absent from the component "
                  "type");
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
    unavailable(ProgramBindingUnresolvedReason::kCapabilityUnavailable,
                "required Program capability has no exact retained provider");
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
        policy.runtime_object_id, std::move(bodies), policy.envelope, policy.bodies_from_motion_endpoint});
  }
  return resolved;
}

}  // namespace

namespace {

using IoSignalRoles =
    std::map<std::pair<core::TargetRoleId, core::ResourceRoleId>,
             core::IoSignalDirection>;
// Every Program-contract requirement of one capability on one target role
// (all catalog entries): the resource roles read when each requirement is a
// Get/Check/Wait Input read of its own input capability on that role; nullopt
// when any requirement comes from another node.
using IoInputReads =
    std::map<std::pair<core::TargetRoleId, core::CapabilityId>,
             std::optional<std::set<core::ResourceRoleId>>>;

// Signal requirements retain their authored resource association; target-level
// capability unions may also contain unrelated coordinate roles.
IoSignalRoles
require_supported_io_signal_roles(const ProgramBindingContract &contract,
                                  IoInputReads *input_reads = nullptr) {
  IoSignalRoles roles;
  // Input-read node instance -> (target role, input capability, resource role).
  std::map<std::string, std::tuple<std::string, std::string, std::string>>
      read_operands;
  const auto *tree =
      contract.normalized_program_root().find_object_member("tree");
  if (tree == nullptr)
    fail("Program has no verified tree");
  std::vector<const CanonicalValue *> pending{tree};
  while (!pending.empty()) {
    const auto *node = pending.back();
    pending.pop_back();
    const auto *id = node->find_object_member("node_id");
    if (id == nullptr || !id->is_string())
      fail("Program node has no verified identity");
    if (id->as_string() == "io.get_input" ||
        id->as_string() == "io.check_input" ||
        id->as_string() == "io.wait_for_input" ||
        id->as_string() == "io.set_output") {
      const auto *parameters = node->find_object_member("parameters");
      const auto *target =
          parameters ? parameters->find_object_member("target_role") : nullptr;
      const auto *resource =
          parameters ? parameters->find_object_member("resource_role")
                     : nullptr;
      if (!target || !target->is_string() || !resource ||
          !resource->is_string())
        fail(
            "io_signal_unbound: I/O roles must be literal verified identities");
      if (id->as_string() == "io.wait_for_input") {
        const auto *edge = parameters->find_object_member("edge");
        if (edge && (!edge->is_string() || edge->as_string() != "level"))
          fail("io_signal_transition_unsupported: only level watches are "
               "installed");
      }
      const auto direction = id->as_string() == "io.set_output"
                                 ? core::IoSignalDirection::kOutput
                                 : core::IoSignalDirection::kInput;
      const auto *instance = node->find_object_member("instance_id");
      if (direction == core::IoSignalDirection::kInput && instance &&
          instance->is_string())
        read_operands[instance->as_string()] = {
            target->as_string(),
            id->as_string() == "io.wait_for_input" ? "io.gpio.wait_for_input"
                                                   : "io.gpio.get_input",
            resource->as_string()};
      if (direction == core::IoSignalDirection::kOutput) {
        const auto *sources = node->find_object_member("value_sources");
        if (sources && (sources->find_object_member("target_role") ||
                        sources->find_object_member("resource_role")))
          fail("io_signal_unbound: output roles must be literal verified "
               "identities");
        const auto &requirements = contract.value().target_roles;
        const auto required_target = std::find_if(
            requirements.begin(), requirements.end(), [&](const auto &role) {
              return role.target_role_id.value() == target->as_string();
            });
        const auto *builtin =
            core::find_builtin_capability_definition("io.gpio.set_output");
        if (required_target == requirements.end() || !builtin ||
            std::none_of(
                required_target->required_capabilities.begin(),
                required_target->required_capabilities.end(),
                [&](const auto &capability) {
                  return capability.capability_id.value() ==
                             builtin->capability_id &&
                         capability.capability_definition_revision.value() ==
                             builtin->capability_definition_revision;
                }) ||
            std::none_of(required_target->required_resource_roles.begin(),
                         required_target->required_resource_roles.end(),
                         [&](const auto &role) {
                           return role.value() == resource->as_string();
                         }))
          fail("io_signal_unbound: output needs its required capability and "
               "resource role");
      }
      const auto [existing, inserted] =
          roles.emplace(std::pair{core::TargetRoleId(target->as_string()),
                                  core::ResourceRoleId(resource->as_string())},
                        direction);
      if (!inserted && existing->second != direction)
        fail("io_signal_direction_mismatch: one signal role cannot be both "
             "read and written");
    }
    const auto condition_roles =
        detail::collect_program_node_io_signal_roles(*node);
    if (!condition_roles.ok())
      fail(condition_roles.error().details.at("reason") + ": " +
           condition_roles.error().message);
    for (const auto &[target_id, resource_id] : condition_roles.value()) {
      const auto &requirements = contract.value().target_roles;
      const auto target = std::find_if(
          requirements.begin(), requirements.end(),
          [&](const auto &row) { return row.target_role_id == target_id; });
      if (target == requirements.end() ||
          std::find(target->required_resource_roles.begin(),
                    target->required_resource_roles.end(),
                    resource_id) == target->required_resource_roles.end())
        fail("io_signal_unbound: condition input needs its exact required "
             "target/resource role");
      const auto [existing, inserted] = roles.emplace(
          std::pair{target_id, resource_id}, core::IoSignalDirection::kInput);
      if (!inserted && existing->second != core::IoSignalDirection::kInput)
        fail("io_signal_direction_mismatch: one signal role cannot be both "
             "read and written");
    }
    if (const auto *children = node->find_object_member("children")) {
      if (!children->is_array())
        fail("Program children are not verified");
      for (const auto &child : children->as_array())
        pending.push_back(&child);
    }
  }
  if (input_reads) {
    for (const auto &node : contract.execution_table().nodes)
      for (const auto &effect : node.target_effects) {
        auto &reads = input_reads
                          ->try_emplace({effect.target_role_id,
                                         effect.capability_id},
                                        std::set<core::ResourceRoleId>{})
                          .first->second;
        const auto read = read_operands.find(node.instance_id.value());
        if (reads && read != read_operands.end() &&
            std::get<0>(read->second) == effect.target_role_id.value() &&
            std::get<1>(read->second) == effect.capability_id.value())
          reads->insert(core::ResourceRoleId(std::get<2>(read->second)));
        else
          reads.reset();
      }
  }
  return roles;
}

struct UnresolvedRoleCollector final {
  std::vector<ProgramBindingUnresolvedRoleDTO> rows;
  std::optional<core::ContentDigest> role_assignment_digest;
  ProgramResolvedRolesDTO resolved;
};

[[nodiscard]] std::vector<RobotTcp> compile_robot_tcps(
    const core::RetainedBundleDTO& bundle,
    const VerifiedRetainedModelMap& retained_model_map,
    const core::ControllableComponentId& robot,
    const core::CommandTargetId& robot_target,
    std::span<const QualifiedCapabilityDefinition> capabilities,
    const ICatalogArtifactSource& catalog_artifact_store);

[[nodiscard]] std::shared_ptr<const ProgramBinding>
compile_program_binding_contract(
    const core::ProgramBindingContractDTO &program_contract,
    core::ProgramRoleAssignmentDTO role_assignment,
    const RetainedBundle &retained_bundle,
    const VerifiedRetainedModelMap &retained_model_map,
    const ICatalogArtifactSource &catalog_artifact_store,
    UnresolvedRoleCollector *collector = nullptr,
    const IoSignalRoles *io_signal_roles = nullptr,
    const IoInputReads *io_input_reads = nullptr) {
  try {
    if (!verify_retained_bundle_identity(retained_bundle) ||
        retained_model_map.query().retained_bundle_id !=
            retained_bundle.retained_bundle_id()) {
      fail("binding compiler inputs do not share one verified retained bundle");
    }
    const auto& bundle = retained_bundle.value();
    const auto io_catalog = ResolveCompiledIoSignalCatalog(bundle.cell_graph);
    if (!io_catalog.ok())
      fail(io_catalog.error().details.at("reason"));
    if (program_contract.node_environment != bundle.catalogs.node_environment) {
      fail("Program contract selects another retained node environment");
    }
    normalize_role_assignment(program_contract, role_assignment);
    if (collector != nullptr) {
      collector->role_assignment_digest =
          role_assignment_digest(role_assignment);
    }
    std::set<core::ComponentRoleId> unresolved_component_roles;
    // Candidate-dependent failures of one role are recorded and the next role
    // is compiled; without a collector they fail exactly as before.
    const auto record = [&](const TargetUnavailable& error,
                            const core::ComponentRoleId& component_role,
                            std::optional<core::TargetRoleId> target_role,
                            const core::ControllableComponentId& component,
                            std::optional<core::CommandTargetId> target) {
      if (collector == nullptr) {
        fail(error.what());
      }
      std::string detail(error.what());
      detail.resize(std::min(detail.size(),
                             kMaximumProgramBindingUnresolvedDetailBytes));
      collector->rows.push_back(ProgramBindingUnresolvedRoleDTO{
          component_role, std::move(target_role), component,
          std::move(target), error.reason(), std::move(detail)});
    };

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
      try {
        const auto& component = require_component(
            bundle.cell_graph, assignment.controllable_component_id);
        const auto key =
            std::make_pair(component.component_type_id.value(),
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
      } catch (const TargetUnavailable& error) {
        record(error, requirement.component_role_id, std::nullopt,
               assignment.controllable_component_id, std::nullopt);
        unresolved_component_roles.insert(requirement.component_role_id);
      }
    }

    std::vector<core::ProgramTargetRoleBindingDTO> target_bindings;
    target_bindings.reserve(program_contract.target_roles.size());
    for (const auto& requirement : program_contract.target_roles) {
      const auto& component_assignment = require_component_assignment(
          role_assignment, requirement.component_role_id);
      const auto& target_assignment =
          require_target_assignment(role_assignment, requirement.target_role_id);
      if (unresolved_component_roles.contains(requirement.component_role_id)) {
        record(TargetUnavailable(
                   ProgramBindingUnresolvedReason::kComponentRoleUnresolved,
                   "target role depends on an unresolved component role"),
               requirement.component_role_id, requirement.target_role_id,
               component_assignment.controllable_component_id,
               target_assignment.command_target_id);
        continue;
      }
      try {
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
            unavailable(ProgramBindingUnresolvedReason::kTargetIncompatible,
                        "target role requires an unavailable component model "
                        "role");
          }
        }

        std::vector<core::ProgramFrameRoleBindingDTO> frames;
        for (const auto& role : requirement.required_frame_roles) {
          const auto* declared_frame =
              detail::declared_target_frame_role(target_type, role);
          // The target's own row (one name per frame, C-RQ8). TCP-C: a custom
          // TCP (TCP-A) is a composed frame of the target that its type does
          // not declare; it binds like a declared frame.
          const auto* frame = find_target_frame_role(*target, role);
          if (declared_frame == nullptr && frame == nullptr) {
            unavailable(ProgramBindingUnresolvedReason::kTargetIncompatible,
                        "target role requires a frame absent from its component "
                        "type");
          }
          if (frame == nullptr) {
            fail("target role requires a missing or ambiguous frame role");
          }
          const auto* resolved =
              std::get_if<CompositionComponentModelFrame>(&frame->frame);
          if (resolved == nullptr ||
              !retained_model_map.kinematic_index().carries(
                  component.controllable_component_id,
                  resolved->controllable_component_id)) {
            fail("motion_endpoint_not_mounted: target frame does not resolve to "
                 "the selected component or a tool mounted on it");
          }
          // Contact bodies stay with the component's own declared frame.
          const bool own_frame = declared_frame != nullptr &&
                                 resolved->controllable_component_id ==
                                     component.controllable_component_id;
          frames.push_back(core::ProgramFrameRoleBindingDTO{
              role, resolved->model_instance_id, resolved->model_revision,
              resolved->compiled_frame_name,
              own_frame ? declared_frame->contact_body_role_ids
                        : std::vector<core::CollisionBodyRoleId>{}});
        }

        // Scene frame resolution is independent of command capability
        // availability. Preserve only these verified placement facts in the
        // explicitly non-executable projection before resolving commands.
        if (collector != nullptr) {
          auto scene_frames = frames;
          for (auto& frame : scene_frames) {
            // This scene-only projection has no collision-role bindings, so
            // endpoint contact-body associations stay on executable rows.
            frame.contact_body_role_ids.clear();
          }
          collector->resolved.target_roles.push_back(
              core::ProgramTargetRoleBindingDTO{
                  requirement.target_role_id, requirement.component_role_id,
                  component.controllable_component_id,
                  target_assignment.command_target_id, std::move(scene_frames),
                  {}, {}, {}});
        }

        std::vector<core::ProgramResourceRoleBindingDTO> resources;
        std::vector<core::ProgramSignalRoleBindingDTO> signals;
        std::set<core::ResourceRoleId> system_signal_roles;
        for (const auto& role : requirement.required_resource_roles) {
          // A compiled system signal (`carried_object_lost` on a gripper
          // target) is a resource role the component type does not declare;
          // it is exempt only when the Program reads it as a signal.
          const bool read_as_signal =
              io_signal_roles &&
              io_signal_roles->contains({requirement.target_role_id, role});
          const bool system_signal = read_as_signal && std::any_of(
              io_catalog.value().begin(), io_catalog.value().end(),
              [&](const auto &value) {
                return value.provider.profile == kSystemCarriedObjectProfile &&
                       value.descriptor.signal_id.controllable_component_id ==
                           component.controllable_component_id &&
                       value.command_target_id ==
                           target_assignment.command_target_id &&
                       value.resource_role_id == role;
              });
          if (!system_signal &&
              std::none_of(target_type.resource_roles.begin(),
                           target_type.resource_roles.end(), [&](const auto& value) {
                             return core::resource_role_id(value) == role;
                           })) {
            unavailable(ProgramBindingUnresolvedReason::kTargetIncompatible,
                        "target role requires a resource absent from its "
                        "component type");
          }
          if (read_as_signal) {
            const auto direction =
                io_signal_roles->at({requirement.target_role_id, role});
            const auto declared = std::find_if(
                target_type.resource_roles.begin(),
                target_type.resource_roles.end(), [&](const auto &value) {
                  return core::resource_role_id(value) == role;
                });
            const auto *local =
                declared == target_type.resource_roles.end()
                    ? nullptr
                    : std::get_if<core::ComponentTypeLocalResourceRoleDTO>(
                          &*declared);
            const auto resolved =
                std::find_if(io_catalog.value().begin(),
                             io_catalog.value().end(), [&](const auto &value) {
                               return value.descriptor.signal_id.controllable_component_id ==
                                          component.controllable_component_id &&
                                      value.command_target_id ==
                                          target_assignment.command_target_id &&
                                      value.resource_role_id == role;
                             });
            if ((!system_signal && (!local || local->coordinate_kind)) ||
                resolved == io_catalog.value().end())
              unavailable(ProgramBindingUnresolvedReason::kTargetIncompatible,
                          direction == core::IoSignalDirection::kInput
                              ? "io_signal_unbound: input role needs one "
                                "declared non-coordinate signal"
                              : "io_signal_unbound: output role needs one "
                                "declared non-coordinate signal");
            if (resolved->descriptor.direction != direction)
              unavailable(ProgramBindingUnresolvedReason::kTargetIncompatible,
                          direction == core::IoSignalDirection::kInput
                              ? "io_signal_direction_mismatch: output has no "
                                "independent input readback"
                              : "io_signal_direction_mismatch: input cannot be "
                                "written");
            const auto digest =
                core::ComputeIoSignalDescriptorDigest(resolved->descriptor);
            if (!digest.ok())
              fail(digest.error().details.at("reason"));
            signals.push_back({role, resolved->descriptor.signal_id.name,
                               resolved->descriptor.value_type,
                               resolved->descriptor.direction,
                               resolved->descriptor.transition_capability,
                               digest.value()});
            if (system_signal)
              system_signal_roles.insert(role);
            continue;
          }
          resources.push_back(resolve_resource_role(
                                  retained_model_map, target_type,
                                  component.controllable_component_id,
                                  target_assignment.command_target_id, role)
                                  .binding);
        }

        std::vector<core::ProgramCollisionBodyRoleBindingDTO> collision_bodies;
        std::set<std::string> retained_collision_role_ids;
        for (const auto& role : requirement.required_collision_body_roles) {
          retained_collision_role_ids.insert(role.value());
        }
        for (const auto& frame : frames) {
          for (const auto& role : frame.contact_body_role_ids) {
            retained_collision_role_ids.insert(role.value());
          }
        }
        for (const auto& role_text : retained_collision_role_ids) {
          const core::CollisionBodyRoleId role(role_text);
          if (std::none_of(target_type.collision_body_roles.begin(),
                           target_type.collision_body_roles.end(),
                           [&](const auto& value) {
                             return value.collision_body_role_id == role;
                           })) {
            unavailable(ProgramBindingUnresolvedReason::kTargetIncompatible,
                        "target role requires a collision body absent from its "
                        "component type");
          }
          const auto& collision = require_collision(*target, role);
          const auto frame_owner = std::find_if(
              frames.begin(), frames.end(), [&](const auto& frame) {
                return std::find(frame.contact_body_role_ids.begin(),
                                 frame.contact_body_role_ids.end(), role) !=
                       frame.contact_body_role_ids.end();
              });
          if (frame_owner != frames.end() &&
              frame_owner->model_instance_id !=
                  collision.model_instance_id) {
            fail("endpoint contact-body role does not resolve through its "
                 "frame model role");
          }
          const auto& model =
              require_model(bundle.cell_graph, collision.model_instance_id);
          if (frame_owner != frames.end() &&
              frame_owner->model_revision != model.model_revision) {
            fail("endpoint contact-body role uses a different model revision "
                 "from its frame");
          }
          collision_bodies.push_back(
              core::ProgramCollisionBodyRoleBindingDTO{
                  role, collision.model_instance_id, model.model_revision,
                  collision.compiled_link_names});
        }

        std::vector<core::ProgramCapabilityBindingDTO> capabilities;
        for (const auto& capability : requirement.required_capabilities) {
          // A target that does not offer the capability is accepted only
          // when every Program requirement of it on this role is an input
          // read whose operand resolves to a compiled system signal (e.g.
          // `carried_object_lost.<target>`, never authored): that signal
          // serves the read through the ordinary signal path. Every other
          // case resolves, and refuses, exactly as before.
          const bool target_offers = std::any_of(
              target_type.capabilities.begin(), target_type.capabilities.end(),
              [&](const auto &value) {
                return value.capability_id == capability.capability_id &&
                       value.capability_definition_revision ==
                           capability.capability_definition_revision;
              });
          // A system-signal read needs no retained capability definition:
          // the read is served through `signal_role_bindings` only.
          if (!target_offers && io_input_reads) {
            const auto reads = io_input_reads->find(
                {requirement.target_role_id, capability.capability_id});
            if (reads != io_input_reads->end() && reads->second &&
                std::all_of(reads->second->begin(), reads->second->end(),
                            [&](const auto &role) {
                              return system_signal_roles.contains(role);
                            }))
              continue;
          }
          auto resolved = resolve_capability(
              capability, target_type, component, bundle.cell_graph, bundle,
              catalog, retained_model_map);
          if (resolved.has_value()) {
            capabilities.push_back(std::move(*resolved));
          }
        }
        std::optional<std::vector<RobotTcp>> target_tcps;
        if (std::any_of(target_type.capabilities.begin(),
                        target_type.capabilities.end(),
                        [](const auto& capability) {
                          return capability.capability_id.value() ==
                                 "execution.trajectory.execute";
                        })) {
          target_tcps = compile_robot_tcps(
              bundle, retained_model_map,
              component.controllable_component_id,
              target_assignment.command_target_id, catalog.capabilities,
              catalog_artifact_store);
        }
        std::optional<core::TcpEntryDTO> default_tcp;
        if (const auto* value = retained_model_map.kinematic_index()
                                    .default_tcp_of(
                                        component.controllable_component_id)) {
          default_tcp = core::TcpEntryDTO{
              component.controllable_component_id,
              value->command_target_id, value->frame_role_id};
        }
        target_bindings.push_back(core::ProgramTargetRoleBindingDTO{
            requirement.target_role_id, requirement.component_role_id,
            component.controllable_component_id,
            target_assignment.command_target_id, std::move(frames),
            std::move(resources), std::move(collision_bodies),
            std::move(capabilities), std::move(signals), std::nullopt,
            std::move(target_tcps),
            std::move(default_tcp)});
      } catch (const TargetUnavailable& error) {
        record(error, requirement.component_role_id, requirement.target_role_id,
               component_assignment.controllable_component_id,
               target_assignment.command_target_id);
      }
    }
    if (collector != nullptr && !collector->rows.empty()) {
      collector->resolved.component_roles = std::move(component_bindings);
      return nullptr;
    }

    // Add only the exact signal roles selected by an authored pneumatic
    // gripper. These are ordinary sealed I/O targets, not caller-selected
    // hardware addresses or an alternate dispatch authority.
    using PneumaticIo = ros2_control_execution_runtime_contract::Configuration::PneumaticIo;
    std::map<std::pair<core::ControllableComponentId, core::CommandTargetId>, PneumaticIo>
        pneumatic_by_target;
    for (const auto &unit : bundle.cell_graph.runtime_units) {
      if (unit.runtime_profile_id.value() != ros2_control_execution_runtime_contract::kRuntimeProfileId)
        continue;
      const auto rows = unit.configuration.find("gripper_target_configurations");
      if (rows == unit.configuration.end()) continue;
      const auto *array = std::get_if<core::JsonValueDTO::Array>(&rows->second.value);
      if (!array) fail("retained gripper configuration is not an array");
      for (const auto &row : detail::decode_ros2_control_execution_gripper_target_configurations(*array)) {
        if (row.pneumatic_io && !pneumatic_by_target.emplace(
              std::pair{row.controllable_component_id, row.command_target_id}, *row.pneumatic_io).second)
          fail("retained pneumatic target has multiple I/O mappings");
      }
    }
    std::vector<core::ProgramComponentRoleBindingDTO> private_components;
    std::vector<core::ProgramTargetRoleBindingDTO> private_targets;
    const auto private_id = [](std::string_view domain, std::string_view first,
                               std::string_view second, std::string_view third) {
      std::string preimage(domain);
      for (const auto part : {first, second, third}) {
        preimage.push_back('\0');
        preimage.append(part);
      }
      return std::string("mf.pneumatic.") + sha256_digest(preimage).substr(7U);
    };
    const auto bind_signal = [&](const core::IoSignalId &id,
                                 const core::IoSignalDirection direction)
        -> std::pair<core::TargetRoleId, core::ResourceRoleId> {
      const auto signal = std::find_if(io_catalog.value().begin(), io_catalog.value().end(),
          [&](const auto &row) { return row.descriptor.signal_id == id; });
      if (signal == io_catalog.value().end() || signal->descriptor.direction != direction)
        fail("pneumatic I/O signal is missing or has the wrong direction");
      const auto wanted = direction == core::IoSignalDirection::kOutput
                              ? "io.gpio.set_output" : "io.gpio.get_input";
      // Reuse an authored target that already resolves this exact signal.
      // Otherwise the same Program would register two aliases for one output
      // and the existing participant resolver would report an ambiguity.
      const auto matches_authored = [&](const auto &row) {
            return row.controllable_component_id == id.controllable_component_id &&
                   row.command_target_id == signal->command_target_id &&
                   std::any_of(row.capability_bindings.begin(), row.capability_bindings.end(),
                       [&](const auto &cap) { return cap.capability_id.value() == wanted; }) &&
                   std::any_of(row.signal_role_bindings.begin(), row.signal_role_bindings.end(),
                       [&](const auto &item) {
                         return item.resource_role_id == signal->resource_role_id &&
                                item.signal_name == id.name && item.direction == direction;
                       });
          };
      const auto authored = std::find_if(target_bindings.begin(), target_bindings.end(), matches_authored);
      if (authored != target_bindings.end() &&
          std::find_if(std::next(authored), target_bindings.end(), matches_authored) != target_bindings.end()) {
        std::string names;
        for (const auto &row : target_bindings)
          if (matches_authored(row))
            names += (names.empty() ? "" : ", ") + row.target_role_id.value();
        fail("pneumatic_io_signal_ambiguous_target: " + id.controllable_component_id.value() +
             "/" + id.name + " matches target roles " + names);
      }
      if (authored != target_bindings.end())
        return {authored->target_role_id, signal->resource_role_id};
      const core::ComponentRoleId component_role(private_id(
          "component", id.controllable_component_id.value(), "", ""));
      const core::TargetRoleId target_role(private_id(
          "signal", id.controllable_component_id.value(), id.name, ""));
      if (std::any_of(component_bindings.begin(), component_bindings.end(),
                      [&](const auto &row) { return row.component_role_id == component_role; }) ||
          std::any_of(target_bindings.begin(), target_bindings.end(),
                      [&](const auto &row) { return row.target_role_id == target_role; }))
        fail("pneumatic private role conflicts with an authored Program role");
      if (std::none_of(private_targets.begin(), private_targets.end(),
                       [&](const auto &row) { return row.target_role_id == target_role; })) {
        const auto direct = compile_direct_control_io_program_binding(
            id.controllable_component_id, signal->command_target_id,
            retained_bundle, retained_model_map, catalog_artifact_store);
        if (direct->value().component_role_bindings.size() != 1U ||
            direct->value().target_role_bindings.size() != 1U)
          fail("pneumatic I/O has no unique direct binding");
        auto component = direct->value().component_role_bindings.front();
        component.component_role_id = component_role;
        auto target = direct->value().target_role_bindings.front();
        target.target_role_id = target_role;
        target.component_role_id = component_role;
        std::erase_if(target.signal_role_bindings,
                      [&](const auto &row) { return row.resource_role_id != signal->resource_role_id; });
        std::erase_if(target.capability_bindings,
                      [&](const auto &row) { return row.capability_id.value() != wanted; });
        if (target.signal_role_bindings.size() != 1U || target.capability_bindings.size() != 1U)
          fail("pneumatic I/O direct binding does not resolve its exact signal");
        if (std::none_of(private_components.begin(), private_components.end(),
                         [&](const auto &row) { return row.component_role_id == component_role; }))
          private_components.push_back(std::move(component));
        private_targets.push_back(std::move(target));
      }
      return {target_role, signal->resource_role_id};
    };
    for (auto &target : target_bindings) {
      const auto selected = pneumatic_by_target.find(
          {target.controllable_component_id, target.command_target_id});
      if (selected == pneumatic_by_target.end() ||
          std::none_of(target.capability_bindings.begin(), target.capability_bindings.end(),
                       [](const auto &row) { return row.capability_id.value() == "tool.gripper.command"; }))
        continue;
      core::ProgramPneumaticIoBindingDTO route;
      for (const auto &row : selected->second.outputs) {
        const auto [target_role, resource_role] =
            bind_signal(row.signal, core::IoSignalDirection::kOutput);
        route.outputs.push_back({target_role, resource_role, row.open_value, row.close_value});
      }
      for (const auto &[name, signal] : selected->second.inputs) {
        const auto [target_role, resource_role] =
            bind_signal(signal, core::IoSignalDirection::kInput);
        route.inputs.push_back({name, target_role, resource_role});
      }
      target.pneumatic_io = std::move(route);
    }
    component_bindings.insert(component_bindings.end(), private_components.begin(), private_components.end());
    target_bindings.insert(target_bindings.end(), private_targets.begin(), private_targets.end());

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
  } catch (const TargetUnavailable& error) {
    // Raised outside a per-role scope: never a non-executable Program.
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

// TCP-C: the TCP frame roles of one command target: the roles its type tags
// as TCPs, plus the composed frames its type does not declare (the
// component's custom TCPs, TCP-A).
[[nodiscard]] std::set<std::string> target_tcp_roles(
    const core::ComponentTypeCommandTargetDTO& type,
    const CompositionTargetResolution* composed) {
  std::set<std::string> roles;
  for (const auto& frame : type.frame_roles) {
    if (frame.tcp) roles.insert(frame.frame_role_id.value());
  }
  if (composed != nullptr) {
    for (const auto& frame : composed->frames) {
      if (std::none_of(type.frame_roles.begin(), type.frame_roles.end(),
                       [&](const auto& declared) {
                         return declared.frame_role_id == frame.frame_role_id;
                       })) {
        roles.insert(frame.frame_role_id.value());
      }
    }
  }
  return roles;
}

[[nodiscard]] bool same_tcp_entry(const core::TcpEntryDTO& left,
                                  const core::TcpEntryDTO& right) {
  return left.component_id == right.component_id &&
         left.target_id == right.target_id &&
         left.frame_role_id == right.frame_role_id;
}

[[nodiscard]] std::vector<RobotTcp> compile_robot_tcps(
    const core::RetainedBundleDTO& bundle,
    const VerifiedRetainedModelMap& retained_model_map,
    const core::ControllableComponentId& robot,
    const core::CommandTargetId& robot_target,
    std::span<const QualifiedCapabilityDefinition> capabilities,
    const ICatalogArtifactSource& catalog_artifact_store) {
  const auto& graph = bundle.cell_graph;
  const auto& index = retained_model_map.kinematic_index();
  const auto end_effector = index.end_effector_entry(robot, robot_target);
  std::vector<RobotTcp> rows;

  const auto append_target = [&](const core::ControllableComponentDTO& owner,
                                 const core::ComponentTypeCommandTargetDTO& target_type,
                                 const bool robot_target_row) {
    const auto* composed = retained_model_map.find_target(
        owner.controllable_component_id, target_type.command_target_id);
    if (composed == nullptr) {
      fail("robot TCP target has no exact retained model-map target");
    }
    auto roles = target_tcp_roles(target_type, composed);
    // The robot's flange, when present, is also in the TCP list.
    if (robot_target_row &&
        find_target_frame_role(*composed, core::FrameRoleId("flange")) != nullptr)
      roles.insert("flange");
    const auto* component_default =
        index.default_tcp_of(owner.controllable_component_id);
    for (const auto& role_name : roles) {
      const core::FrameRoleId role(role_name);
      const auto* frame = find_target_frame_role(*composed, role);
      if (frame == nullptr) {
        fail("robot TCP role has no exact retained frame");
      }
      const auto* resolved =
          std::get_if<CompositionComponentModelFrame>(&frame->frame);
      if (resolved == nullptr ||
          resolved->controllable_component_id !=
              owner.controllable_component_id) {
        fail("robot TCP frame does not belong to its declared owner");
      }
      const auto* declared =
          detail::declared_target_frame_role(target_type, role);
      std::vector<core::CollisionBodyRoleId> contact_bodies;
      if (declared != nullptr)
        contact_bodies = declared->contact_body_role_ids;
      const core::TcpEntryDTO entry{owner.controllable_component_id,
                                    target_type.command_target_id, role};
      rows.push_back(RobotTcp{
          entry,
          core::ProgramFrameRoleBindingDTO{
              role, resolved->model_instance_id, resolved->model_revision,
              resolved->compiled_frame_name, std::move(contact_bodies)},
          end_effector.has_value() && same_tcp_entry(*end_effector, entry),
          component_default != nullptr &&
              component_default->command_target_id == entry.target_id &&
              component_default->frame_role_id == entry.frame_role_id});
    }
  };

  const auto& robot_component = require_component(graph, robot);
  const auto robot_type = load_component_type(
      bundle, robot_component, capabilities, catalog_artifact_store);
  const auto& robot_command_target = require_target_type(robot_type, robot_target);
  append_target(robot_component, robot_command_target, true);

  std::vector<const core::ControllableComponentDTO*> components;
  components.reserve(graph.controllable_components.size());
  for (const auto& component : graph.controllable_components)
    components.push_back(&component);
  std::sort(components.begin(), components.end(), [](const auto* left,
                                                      const auto* right) {
    return left->controllable_component_id < right->controllable_component_id;
  });
  for (const auto* component : components) {
    if (component->controllable_component_id == robot) continue;
    const auto type = load_component_type(bundle, *component, capabilities,
                                          catalog_artifact_store);
    if (!contains(type.value().tags, "tool")) continue;
    std::vector<const core::ComponentTypeCommandTargetDTO*> targets;
    targets.reserve(type.value().command_targets.size());
    for (const auto& target : type.value().command_targets)
      targets.push_back(&target);
    std::sort(targets.begin(), targets.end(), [](const auto* left,
                                                 const auto* right) {
      return left->command_target_id < right->command_target_id;
    });
    for (const auto* target : targets)
      append_target(*component, *target, false);
  }

  std::sort(rows.begin(), rows.end(), [](const auto& left, const auto& right) {
    return std::tie(left.entry.component_id, left.entry.target_id,
                    left.entry.frame_role_id) <
           std::tie(right.entry.component_id, right.entry.target_id,
                    right.entry.frame_role_id);
  });
  if (rows.size() > core::kMaxProgramNestedRoleBindings ||
      std::adjacent_find(rows.begin(), rows.end(), [](const auto& left,
                                                       const auto& right) {
        return same_tcp_entry(left.entry, right.entry);
      }) != rows.end()) {
    fail("robot TCP list is duplicate or exceeds its fixed bound");
  }
  return rows;
}

// TCP-C: the tool on a robot's `flange` interface, its TCP target (the
// target of its default TCP, B5) and that target's TCP roles.
struct DirectControlMountedTool final {
  core::ControllableComponentId controllable_component_id;
  core::CommandTargetId command_target_id;
  std::set<std::string> model_roles;
  std::set<std::string> tcp_roles;
};

[[nodiscard]] std::optional<DirectControlMountedTool> direct_control_mounted_tool(
    const core::RetainedBundleDTO& bundle,
    const VerifiedRetainedModelMap& retained_model_map,
    const core::ControllableComponentId& robot,
    std::span<const QualifiedCapabilityDefinition> capabilities,
    const ICatalogArtifactSource& catalog_artifact_store) {
  // The single or primary tool on the `flange` interface (B5).
  const auto& index = retained_model_map.kinematic_index();
  const auto* tool_id = index.tool_on(robot, "flange");
  if (tool_id == nullptr) return std::nullopt;
  // A tool without a default TCP has no TCP (C-P2).
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
  result.tcp_roles = target_tcp_roles(
      target, retained_model_map.find_target(tool.controllable_component_id,
                                             target.command_target_id));
  return result;
}

[[nodiscard]] core::ProgramBindingContractDTO
make_direct_control_motion_contract(
    const core::RetainedNodeEnvironmentDTO& node_environment,
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
  // TCP-C interim legacy endpoint binding: keep `base` and the current robot
  // plus mounted-tool TCP roles. The full Deployment inventory is compiled
  // separately into `robot_tcps`; other frame roles, a merely named `tcp`
  // included (C-P3), are not endpoint bindings.
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
  // TCP-C: a mounted tool's TCPs are a frame-only second target role, so
  // Robot Control can move the robot at them ({mounted_tool_target, <tcp>}).
  // Without a tool the projection, and so the revision, is unchanged.
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
            {"component_role_id",
             CanonicalValue(std::string(kDirectControlToolComponentRoleId))},
            {"frame_roles", strong_id_array(tool_frame_roles)},
            {"model_roles", strong_id_array(tool_model_roles)},
            {"target_role_id",
             CanonicalValue(std::string(kDirectControlToolTargetRoleId))},
        }));
    revision_projection = CanonicalValue(std::move(fields));
  }
  std::string revision_preimage{
      "manyforge.direct-control-motion-program-revision.v2"};
  revision_preimage.push_back('\0');
  revision_preimage.append(canonical_json(revision_projection));

  core::ProgramBindingContractDTO contract{
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
  if (tool.has_value()) {
    // Role ids sort after the robot's, so the robot stays the first row.
    contract.component_roles.push_back(core::ProgramComponentRoleRequirementDTO{
        core::ComponentRoleId(std::string(kDirectControlToolComponentRoleId)),
        {}, tool_model_roles, {}});
    contract.target_roles.push_back(core::ProgramTargetRoleRequirementDTO{
        core::TargetRoleId(std::string(kDirectControlToolTargetRoleId)),
        core::ComponentRoleId(std::string(kDirectControlToolComponentRoleId)),
        {}, tool_frame_roles, {}, {}, tool_model_roles});
  }
  return contract;
}

// TCP-C: the direct-control role assignment: the robot target, plus the
// mounted tool's TCP target when the contract has one.
[[nodiscard]] core::ProgramRoleAssignmentDTO direct_control_motion_assignment(
    const core::ControllableComponentId& controllable_component_id,
    const core::CommandTargetId& command_target_id,
    const std::optional<DirectControlMountedTool>& tool) {
  core::ProgramRoleAssignmentDTO assignment{
      {core::ProgramComponentRoleAssignmentDTO{
          core::ComponentRoleId(std::string(kDirectControlComponentRoleId)),
          controllable_component_id}},
      {core::ProgramTargetRoleAssignmentDTO{
          core::TargetRoleId(std::string(kDirectControlTargetRoleId)),
          command_target_id}}};
  if (tool.has_value()) {
    assignment.component_roles.push_back(core::ProgramComponentRoleAssignmentDTO{
        core::ComponentRoleId(std::string(kDirectControlToolComponentRoleId)),
        tool->controllable_component_id});
    assignment.target_roles.push_back(core::ProgramTargetRoleAssignmentDTO{
        core::TargetRoleId(std::string(kDirectControlToolTargetRoleId)),
        tool->command_target_id});
  }
  return assignment;
}

[[nodiscard]] core::ProgramBindingContractDTO make_direct_control_io_contract(
    const core::RetainedNodeEnvironmentDTO &node_environment,
    const core::ComponentTypeCommandTargetDTO &target,
    const std::vector<core::ResourceRoleId> &resource_roles,
    bool include_inputs, bool include_outputs) {
  const auto *capability = find_target_capability(
      target, include_inputs ? "io.gpio.get_input" : "io.gpio.set_output");
  const auto *output =
      include_outputs ? find_target_capability(target, "io.gpio.set_output")
                      : nullptr;
  if (!capability || (include_outputs && !output) || resource_roles.empty())
    fail("direct-control input target has no qualified input-read contract");
  auto projection_fields = CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.direct_control_io_program.v1")},
      {"program_id", CanonicalValue(std::string(kDirectControlIoProgramId))},
      {"component_role_id",
       CanonicalValue(std::string(kDirectControlComponentRoleId))},
      {"target_role_id",
       CanonicalValue(std::string(kDirectControlTargetRoleId))},
      {"capability_id", CanonicalValue(capability->capability_id.value())},
      {"capability_definition_revision",
       CanonicalValue(capability->capability_definition_revision.value())},
      {"resource_roles", strong_id_array(resource_roles)},
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
  std::string preimage("manyforge.direct-control-io-program-revision.v1");
  preimage.push_back('\0');
  preimage.append(canonical_json(projection));
  return {core::ProgramId(std::string(kDirectControlIoProgramId)),
          core::ProgramRevision(sha256_digest(preimage)),
          node_environment,
          {{core::ComponentRoleId(std::string(kDirectControlComponentRoleId)),
            {},
            {},
            {}}},
          {{core::TargetRoleId(std::string(kDirectControlTargetRoleId)),
            core::ComponentRoleId(std::string(kDirectControlComponentRoleId)),
            std::move(capabilities),
            {},
            resource_roles,
            {},
            {}}},
          {}};
}

[[nodiscard]] core::ProgramBindingContractDTO
make_direct_control_gripper_contract(
    const core::RetainedNodeEnvironmentDTO &node_environment,
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

std::shared_ptr<const ProgramBinding> compile_program_binding(
    const ProgramBindingContract& program_contract,
    core::ProgramRoleAssignmentDTO role_assignment,
    const RetainedBundle& retained_bundle,
    const VerifiedRetainedModelMap& retained_model_map,
    const ICatalogArtifactSource& catalog_artifact_store) {
  IoInputReads io_input_reads;
  const auto io_signal_roles =
      require_supported_io_signal_roles(program_contract, &io_input_reads);
  return compile_program_binding_contract(
      program_contract.value(), std::move(role_assignment), retained_bundle,
      retained_model_map, catalog_artifact_store, nullptr, &io_signal_roles,
      &io_input_reads);
}

ProgramBindingOutcome compile_program_binding_outcome(
    const ProgramBindingContract& program_contract,
    core::ProgramRoleAssignmentDTO role_assignment,
    const RetainedBundle& retained_bundle,
    const VerifiedRetainedModelMap& retained_model_map,
    const ICatalogArtifactSource& catalog_artifact_store) {
  UnresolvedRoleCollector collector;
  IoInputReads io_input_reads;
  const auto io_signal_roles =
      require_supported_io_signal_roles(program_contract, &io_input_reads);
  auto binding = compile_program_binding_contract(
      program_contract.value(), std::move(role_assignment), retained_bundle,
      retained_model_map, catalog_artifact_store, &collector, &io_signal_roles,
      &io_input_reads);
  if (binding != nullptr) {
    if (!collector.rows.empty()) {
      fail("Program Binding outcome is both bound and unresolved");
    }
    return {std::move(binding), std::nullopt, std::nullopt};
  }
  if (collector.rows.empty() || !collector.role_assignment_digest.has_value()) {
    fail("Program Binding outcome has neither a binding nor a reason");
  }
  std::sort(collector.rows.begin(), collector.rows.end(),
            [](const auto& lhs, const auto& rhs) {
              const auto key = [](const ProgramBindingUnresolvedRoleDTO& row) {
                return std::make_tuple(row.component_role_id.value(),
                                       row.target_role_id.has_value(),
                                       row.target_role_id.has_value()
                                           ? row.target_role_id->value()
                                           : std::string{});
              };
              return key(lhs) < key(rhs);
            });
  std::vector<core::TargetRoleId> target_roles;
  for (const auto& row : collector.rows) {
    if (row.target_role_id.has_value()) {
      target_roles.push_back(*row.target_role_id);
    }
  }
  auto affected = program_binding_affected_scope(
      program_contract.execution_table(), target_roles);
  const auto& bundle = retained_bundle.value();
  ProgramNonExecutableOutcomeDTO outcome{
      program_contract.value().program_id,
      program_contract.value().program_revision,
      retained_bundle.retained_bundle_id(),
      bundle.cell_graph.graph_revision,
      *collector.role_assignment_digest,
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
    core::ProgramRoleAssignmentDTO role_assignment,
    const RetainedBundle& retained_bundle,
    const std::optional<VerifiedRetainedModelMap>& retained_model_map,
    const ICatalogArtifactSource& catalog_artifact_store) {
  if (retained_model_map.has_value()) {
    return compile_program_binding_outcome(
        program_contract, std::move(role_assignment), retained_bundle,
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
  normalize_role_assignment(program_contract.value(), role_assignment);
  // Keep the same strict retained-catalog qualification as the ordinary
  // compiler. Missing references may become a named outcome; corrupt or
  // ambiguous candidate facts still abort preparation.
  static_cast<void>(load_qualified_catalog_closure(
      bundle, catalog_artifact_store));
  static_cast<void>(detail::BuildQualifiedRetentionAuthority::qualify());

  UnresolvedRoleCollector collector;
  collector.role_assignment_digest = role_assignment_digest(role_assignment);
  std::set<core::ComponentRoleId> unresolved_component_roles;
  for (const auto& requirement : program_contract.value().component_roles) {
    const auto& assignment = require_component_assignment(
        role_assignment, requirement.component_role_id);
    try {
      static_cast<void>(require_component(
          bundle.cell_graph, assignment.controllable_component_id));
      // A component without its required retained model map is not an
      // intentional candidate-dependent absence.
      fail("candidate component has no exact retained model map");
    } catch (const TargetUnavailable& error) {
      std::string detail(error.what());
      detail.resize(std::min(detail.size(),
                             kMaximumProgramBindingUnresolvedDetailBytes));
      collector.rows.push_back(ProgramBindingUnresolvedRoleDTO{
          requirement.component_role_id, std::nullopt,
          assignment.controllable_component_id, std::nullopt, error.reason(),
          std::move(detail)});
      unresolved_component_roles.insert(requirement.component_role_id);
    }
  }
  for (const auto& requirement : program_contract.value().target_roles) {
    const auto& component_assignment = require_component_assignment(
        role_assignment, requirement.component_role_id);
    const auto& target_assignment =
        require_target_assignment(role_assignment, requirement.target_role_id);
    if (!unresolved_component_roles.contains(requirement.component_role_id)) {
      fail("candidate target has no exact retained model map");
    }
    constexpr std::string_view detail =
        "target role depends on an unresolved component role";
    collector.rows.push_back(ProgramBindingUnresolvedRoleDTO{
        requirement.component_role_id, requirement.target_role_id,
        component_assignment.controllable_component_id,
        target_assignment.command_target_id,
        ProgramBindingUnresolvedReason::kComponentRoleUnresolved,
        std::string(detail)});
  }
  if (collector.rows.empty()) {
    fail("an empty candidate cannot bind a Program without model data");
  }
  std::sort(collector.rows.begin(), collector.rows.end(),
            [](const auto& lhs, const auto& rhs) {
              const auto key = [](const auto& row) {
                return std::make_tuple(row.component_role_id.value(),
                                       row.target_role_id.has_value(),
                                       row.target_role_id.has_value()
                                           ? row.target_role_id->value()
                                           : std::string{});
              };
              return key(lhs) < key(rhs);
            });
  std::vector<core::TargetRoleId> target_roles;
  for (const auto& row : collector.rows) {
    if (row.target_role_id.has_value()) {
      target_roles.push_back(*row.target_role_id);
    }
  }
  const auto affected = program_binding_affected_scope(
      program_contract.execution_table(), target_roles);
  ProgramNonExecutableOutcomeDTO outcome{
      program_contract.value().program_id,
      program_contract.value().program_revision,
      retained_bundle.retained_bundle_id(),
      bundle.cell_graph.graph_revision,
      *collector.role_assignment_digest,
      std::move(collector.rows),
      affected.nodes,
      affected.execution_groups,
  };
  if (!outcome.valid()) {
    fail("Program Binding produced an invalid non-executable outcome");
  }
  return {nullptr, std::move(outcome), ProgramResolvedRolesDTO{}};
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
        bundle.catalogs.node_environment, target,
        target_tcp_roles(target, retained_model_map.find_target(
                                     controllable_component_id,
                                     command_target_id)),
        tool);
    return compile_program_binding_contract(
        contract,
        direct_control_motion_assignment(controllable_component_id,
                                         command_target_id, tool),
        retained_bundle, retained_model_map, catalog_artifact_store);
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
    const auto bound = std::find_if(targets.begin(), targets.end(), [](const auto &row) {
      return row.target_role_id.value() == kDirectControlTargetRoleId;
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
    IoSignalRoles signal_roles;
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
        signal_roles.emplace(std::pair{core::TargetRoleId(std::string(
                                           kDirectControlTargetRoleId)),
                                       signal.resource_role_id},
                             signal.descriptor.direction);
      }
    const auto roles = sorted_ids<core::ResourceRoleId>(role_names);
    const auto contract = make_direct_control_io_contract(
        bundle.catalogs.node_environment, target, roles, include_inputs,
        include_outputs);
    return compile_program_binding_contract(
        contract,
        {{{core::ComponentRoleId(std::string(kDirectControlComponentRoleId)),
           controllable_component_id}},
         {{core::TargetRoleId(std::string(kDirectControlTargetRoleId)),
           command_target_id}}},
        retained_bundle, retained_model_map, catalog_artifact_store, nullptr,
        &signal_roles);
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
              bundle.catalogs.node_environment, target,
              target_tcp_roles(target, retained_model_map.find_target(
                                           component.controllable_component_id,
                                           target.command_target_id)),
              tool);
          result.motion.push_back(compile_program_binding_contract(
              contract,
              direct_control_motion_assignment(component.controllable_component_id,
                                               target.command_target_id, tool),
              retained_bundle, retained_model_map,
              catalog_artifact_store));
        }
        if (find_target_capability(target, "io.gpio.get_input") != nullptr ||
            find_target_capability(target, "io.gpio.set_output") != nullptr) {
          result.io.push_back(compile_direct_control_io_program_binding(
              component.controllable_component_id, target.command_target_id,
              retained_bundle, retained_model_map, catalog_artifact_store));
        }
        if (find_target_capability(target, "tool.gripper.command") != nullptr) {
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
  if (retained_model_map.query().retained_bundle_id !=
          retained_bundle.retained_bundle_id() ||
      direct_control_binding.value().retained_bundle_id !=
          retained_bundle.retained_bundle_id()) {
    fail("direct-control TCP projection inputs do not share one retained bundle");
  }
  const auto& binding = direct_control_binding.value();
  const auto robot = std::find_if(
      binding.target_role_bindings.begin(), binding.target_role_bindings.end(),
      [](const auto& row) {
        return row.target_role_id.value() == kDirectControlTargetRoleId;
      });
  if (robot == binding.target_role_bindings.end()) {
    fail("direct-control motion binding has no robot target role");
  }
  const auto& index = retained_model_map.kinematic_index();
  std::vector<DirectControlTcp> rows;
  if (!robot->robot_tcps) return rows;
  rows.reserve(robot->robot_tcps->size());
  for (const auto& tcp : *robot->robot_tcps) {
    const bool mounted_here =
        index.carries(robot->controllable_component_id, tcp.entry.component_id);
    std::optional<core::TargetRoleId> legacy_target_role;
    bool ambiguous_legacy_target_role = false;
    for (const auto& target : binding.target_role_bindings) {
      if (target.controllable_component_id != tcp.entry.component_id ||
          target.command_target_id != tcp.entry.target_id) {
        continue;
      }
      const auto frame = std::find_if(
          target.frame_role_bindings.begin(), target.frame_role_bindings.end(),
          [&](const auto& candidate) {
            return candidate.frame_role_id == tcp.entry.frame_role_id &&
                   candidate.model_instance_id == tcp.frame.model_instance_id &&
                   candidate.model_revision == tcp.frame.model_revision &&
                   candidate.compiled_frame_name ==
                       tcp.frame.compiled_frame_name;
          });
      if (frame == target.frame_role_bindings.end()) continue;
      if (legacy_target_role.has_value()) {
        ambiguous_legacy_target_role = true;
        break;
      }
      legacy_target_role = target.target_role_id;
    }
    if (ambiguous_legacy_target_role) legacy_target_role.reset();
    const bool mounted_elsewhere =
        !mounted_here &&
        std::any_of(inventory.motion.begin(), inventory.motion.end(),
                    [&](const auto& candidate) {
                      if (candidate == nullptr) return false;
                      const auto& other_targets =
                          candidate->value().target_role_bindings;
                      const auto other_robot = std::find_if(
                          other_targets.begin(), other_targets.end(),
                          [](const auto& target) {
                            return target.target_role_id.value() ==
                                   kDirectControlTargetRoleId;
                          });
                      if (other_robot == other_targets.end() ||
                          other_robot->controllable_component_id ==
                              robot->controllable_component_id)
                        return false;
                      return index.carries(
                          other_robot->controllable_component_id,
                          tcp.entry.component_id);
                    });
    rows.push_back(DirectControlTcp{
        tcp,
        tcp.entry.component_id.value() + "." +
            tcp.entry.frame_role_id.value(),
        std::move(legacy_target_role), mounted_here, mounted_elsewhere});
  }
  std::sort(rows.begin(), rows.end(), [](const auto& left, const auto& right) {
    if (left.binding.end_effector != right.binding.end_effector)
      return left.binding.end_effector;
    return std::tie(left.binding.entry.component_id,
                    left.binding.entry.target_id,
                    left.binding.entry.frame_role_id) <
           std::tie(right.binding.entry.component_id,
                    right.binding.entry.target_id,
                    right.binding.entry.frame_role_id);
  });
  std::map<std::pair<std::string, std::string>, std::size_t> role_counts;
  for (const auto& row : rows) {
    ++role_counts[{row.binding.entry.component_id.value(),
                   row.binding.entry.frame_role_id.value()}];
  }
  for (auto& row : rows) {
    if (role_counts.at({row.binding.entry.component_id.value(),
                        row.binding.entry.frame_role_id.value()}) > 1U) {
      row.display += " (" + row.binding.entry.target_id.value() + ")";
    }
  }
  return rows;
}

}  // namespace manyforge::deployment
