#include "program_reference_probe_internal.hpp"
#include "program_binding_internal.hpp"
#include "manyforge_deployment/cell_kinematic_index.hpp"

#include <tuple>

namespace manyforge::deployment::reference_probe {

[[noreturn]] void fail(std::string message) {
  throw ProgramBindingCompilerError(std::move(message));
}

/// A role does not resolve only because of a candidate-dependent fact. The
/// ordinary compiler reports it as ProgramBindingCompilerError with the same
/// message; the outcome compiler records it as a named unresolved role.


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

[[nodiscard]] core::ProgramComponentBindingDTO resolve_component_binding(
    const core::ControllableComponentId& component_id,
    const core::CellGraphDTO& graph,
    const QualifiedComponentType& component_type) {
  const auto& component = require_component(graph, component_id);
  const auto& type = component_type.value();

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
  return core::ProgramComponentBindingDTO{
      component.controllable_component_id,
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

/// A selected control capability consumes the exact resource rows of its
/// retained control binding, as does runtime participant construction.
[[nodiscard]] std::set<core::ResourceRoleId> capability_resource_roles(
    const core::ProgramCapabilityBindingDTO& capability,
    const core::ProgramTargetReferenceDTO& target,
    const core::CellGraphDTO& graph) {
  std::set<core::ResourceRoleId> roles;
  if (!capability.control_binding) return roles;
  const auto& reference = *capability.control_binding;
  const auto* binding = find_control_binding(graph, reference.control_binding_id);
  if (!binding || binding->control_binding_revision != reference.control_binding_revision ||
      binding->controllable_component_id != target.component_id ||
      binding->command_target_id != target.command_target_id ||
      binding->capability_id != capability.capability_id ||
      binding->capability_definition_revision != capability.capability_definition_revision)
    fail("selected capability has no exact retained control binding");
  for (const auto& resource : binding->resource_bindings)
    roles.insert(resource.resource_role_id);
  return roles;
}

[[nodiscard]] std::set<core::ResourceRoleId> signal_resource_roles(
    const core::ProgramTargetReferenceDTO& target,
    std::span<const core::ProgramSignalReferenceDTO> references,
    std::span<const RetainedIoSignalConfiguration> catalog) {
  std::set<core::ResourceRoleId> roles;
  for (const auto& reference : references) {
    const auto matches = FindProgramSignalMatches(catalog, reference);
    if (matches.size() != 1U || reference.component_id != target.component_id ||
        matches.front().retained->command_target_id != target.command_target_id)
      fail("Program signal has no unique retained association to its target");
    roles.insert(matches.front().retained->resource_role_id);
  }
  return roles;
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

[[nodiscard]] std::vector<core::ResourceRoleId> command_schema_resource_roles(
    const QualifiedCommandSchema &schema,
    const core::ComponentTypeCommandTargetDTO &target_type) {
  std::vector<core::ResourceRoleId> selected_roles;
  if (schema.value().resource_role_selectors.empty()) {
    selected_roles.reserve(target_type.resource_roles.size());
    for (const auto& role : target_type.resource_roles)
      selected_roles.push_back(core::resource_role_id(role));
  } else {
    selected_roles = schema.value().resource_role_selectors;
  }
  if (selected_roles.empty())
    fail("command schema selected no command-target resource roles");
  return selected_roles;
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

  const auto selected_roles = command_schema_resource_roles(schema, target_type);

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



[[nodiscard]] std::vector<core::ProgramTargetFrameAssociationDTO>
resolve_target_frame_associations(
    const core::ProgramTargetReferenceDTO& program_target,
    std::span<const core::ProgramFrameReferenceDTO> frame_references,
    const core::ComponentTypeCommandTargetDTO& target_type,
    const CompositionTargetResolution& target,
    const VerifiedRetainedModelMap& model_map,
    std::span<const core::ProgramFrameBindingDTO> frames) {
  std::vector<core::ProgramTargetFrameAssociationDTO> result;
  for (const auto& reference : frame_references) {
    const auto* qualified = find_exactly_one<core::ProgramFrameBindingDTO>(
        frames, [&](const auto& frame) { return frame.reference == reference; });
    if (!qualified)
      fail("target frame has no unique qualified Program frame binding");
    std::optional<CompositionComponentModelFrame> selected;
    std::optional<std::vector<core::CollisionBodyRoleId>> contacts;
    for (const auto& candidate : target.frames) {
      const auto* resolved = std::get_if<CompositionComponentModelFrame>(&candidate.frame);
      // A component alias and a model frame ID may name the same physical
      // frame. Match the unique qualified physical binding, while retaining
      // this target's existing frame and mounted-owner constraints.
      if (!resolved || resolved->controllable_component_id != reference.component_id ||
          resolved->model_instance_id != qualified->model_instance_id ||
          resolved->model_revision != qualified->model_revision ||
          resolved->compiled_frame_name != qualified->compiled_frame_name) continue;
      const auto* declared = detail::declared_target_frame_role(target_type, candidate.frame_role_id);
      std::vector<core::CollisionBodyRoleId> candidate_contacts;
      if (declared && reference.component_id == program_target.component_id)
        candidate_contacts = declared->contact_body_role_ids;
      if (!selected) {
        selected = *resolved;
        contacts = std::move(candidate_contacts);
      } else if (contacts->size() != candidate_contacts.size() ||
                 !std::is_permutation(contacts->begin(), contacts->end(),
                                      candidate_contacts.begin())) {
        // Different retained names for this same qualified physical frame
        // agree only if their target-local contact sets agree.
        fail("target frame has conflicting retained aliases or contact bodies");
      }
    }
    if (!selected || !model_map.kinematic_index().carries(
                         program_target.component_id, reference.component_id))
      unavailable(ProgramBindingUnresolvedReason::kTargetIncompatible,
                  "target frame is missing, ambiguous or not mounted");
    std::sort(contacts->begin(), contacts->end());
    result.push_back({reference, std::move(*contacts)});
  }
  return result;
}


// The TCP frame roles of one command target: the roles its type tags
// as TCPs, plus the composed frames its type does not declare (the
// component's custom TCPs).
[[nodiscard]] std::set<std::string> target_tcp_roles(
    const core::ComponentTypeCommandTargetDTO& type,
    const CompositionTargetResolution* composed, bool robot_target_row) {
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
  // A robot's flange is selectable even without a TCP tag.
  if (robot_target_row && composed &&
      find_target_frame_role(*composed, core::FrameRoleId("flange")) != nullptr)
    roles.insert("flange");
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
    auto roles = target_tcp_roles(target_type, composed, robot_target_row);
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
      std::sort(contact_bodies.begin(), contact_bodies.end());
      const core::TcpEntryDTO physical{owner.controllable_component_id,
                                       target_type.command_target_id, role};
      const core::TcpEntryReferenceDTO entry{
          {owner.controllable_component_id, target_type.command_target_id},
          {owner.controllable_component_id, role}};
      rows.push_back(RobotTcp{
          entry,
          core::ProgramFrameBindingDTO{
              entry.frame, resolved->model_instance_id, resolved->model_revision,
              resolved->compiled_frame_name},
          std::move(contact_bodies),
          end_effector.has_value() && same_tcp_entry(*end_effector, physical),
          component_default != nullptr &&
              component_default->command_target_id == physical.target_id &&
              component_default->frame_role_id == physical.frame_role_id});
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
    return std::tie(left.entry.target, left.entry.frame) <
           std::tie(right.entry.target, right.entry.frame);
  });
  if (rows.size() > core::kMaxProgramNestedRoleBindings ||
      std::adjacent_find(rows.begin(), rows.end(), [](const auto& left,
                                                       const auto& right) {
        return left.entry == right.entry;
      }) != rows.end()) {
    fail("robot TCP list is duplicate or exceeds its fixed bound");
  }
  return rows;
}


} // namespace manyforge::deployment::reference_probe

namespace manyforge::deployment {
using namespace reference_probe;
std::optional<ProgramBindingUnresolvedReason> probe_program_target(
    const core::ControllableComponentId& component_id,
    const core::CommandTargetId& target_id,
    const RetainedBundle& retained_bundle,
    const ICatalogArtifactSource& catalog_artifact_store) {
  if (!verify_retained_bundle_identity(retained_bundle)) {
    fail("reference target input is not a verified retained bundle");
  }
  try {
    const auto& bundle = retained_bundle.value();
    const auto catalog =
        load_qualified_catalog_closure(bundle, catalog_artifact_store);
    const auto& component = require_component(bundle.cell_graph, component_id);
    const auto component_type = load_component_type(
        bundle, component, catalog.capabilities, catalog_artifact_store);
    static_cast<void>(require_target_type(component_type, target_id));
    return std::nullopt;
  } catch (const TargetUnavailable& error) {
    return error.reason();
  }
}

core::ComponentTypeDTO qualified_program_component_type(
    const core::ControllableComponentId& component_id,
    const RetainedBundle& retained_bundle,
    const ICatalogArtifactSource& catalog_artifact_store) {
  if (!verify_retained_bundle_identity(retained_bundle))
    fail("selector input is not a verified retained bundle");
  const auto& bundle = retained_bundle.value();
  const auto catalog = load_qualified_catalog_closure(bundle, catalog_artifact_store);
  const auto& component = require_component(bundle.cell_graph, component_id);
  return load_component_type(bundle, component, catalog.capabilities,
                             catalog_artifact_store).value();
}

std::optional<ProgramBindingUnresolvedReason> probe_program_target_capability(
    const core::ControllableComponentId& component_id,
    const core::CommandTargetId& target_id,
    const core::ProgramCapabilityRequirementDTO& requirement,
    const RetainedBundle& retained_bundle,
    const VerifiedRetainedModelMap& retained_model_map,
    const ICatalogArtifactSource& catalog_artifact_store) {
  if (requirement.disposition != core::ProgramCapabilityDisposition::kRequired) {
    fail("reference capability probe requires a required capability");
  }
  if (!verify_retained_bundle_identity(retained_bundle) ||
      retained_model_map.query().retained_bundle_id !=
          retained_bundle.retained_bundle_id()) {
    fail("reference capability inputs do not share one verified retained "
         "bundle");
  }
  try {
    const auto& bundle = retained_bundle.value();
    const auto catalog =
        load_qualified_catalog_closure(bundle, catalog_artifact_store);
    const auto& component = require_component(bundle.cell_graph, component_id);
    const auto component_type = load_component_type(
        bundle, component, catalog.capabilities, catalog_artifact_store);
    const auto& target = require_target_type(component_type, target_id);
    if (retained_model_map.find_target(component_id, target_id) == nullptr) {
      fail("target has no exact retained model-map target");
    }
    static_cast<void>(resolve_capability(requirement, target, component,
                                         bundle.cell_graph, bundle, catalog,
                                         retained_model_map));
    return std::nullopt;
  } catch (const TargetUnavailable& error) {
    return error.reason();
  }
}

std::vector<std::optional<ProgramBindingUnresolvedReason>>
probe_program_target_capabilities(
    const core::ControllableComponentId& component_id,
    const core::CommandTargetId& target_id,
    std::span<const core::ProgramCapabilityRequirementDTO> requirements,
    const RetainedBundle& retained_bundle,
    const VerifiedRetainedModelMap& retained_model_map,
    const ICatalogArtifactSource& catalog_artifact_store) {
  if (!verify_retained_bundle_identity(retained_bundle) ||
      retained_model_map.query().retained_bundle_id != retained_bundle.retained_bundle_id())
    fail("reference capability inputs do not share one verified retained bundle");
  for (const auto& requirement : requirements)
    if (requirement.disposition != core::ProgramCapabilityDisposition::kRequired)
      fail("reference capability probe requires required capabilities");
  const auto& bundle = retained_bundle.value();
  const auto catalog = load_qualified_catalog_closure(bundle, catalog_artifact_store);
  std::vector<std::optional<ProgramBindingUnresolvedReason>> result;
  result.reserve(requirements.size());
  try {
    const auto& component = require_component(bundle.cell_graph, component_id);
    const auto type = load_component_type(bundle, component, catalog.capabilities,
                                          catalog_artifact_store);
    const auto& target = require_target_type(type, target_id);
    if (!retained_model_map.find_target(component_id, target_id))
      fail("target has no exact retained model-map target");
    for (const auto& requirement : requirements) {
      try {
        static_cast<void>(resolve_capability(requirement, target, component,
                                             bundle.cell_graph, bundle, catalog,
                                             retained_model_map));
        result.push_back(std::nullopt);
      } catch (const TargetUnavailable& error) {
        result.push_back(error.reason());
      }
    }
  } catch (const TargetUnavailable& error) {
    result.assign(requirements.size(), error.reason());
  }
  return result;
}

bool probe_program_cell_capability(
    const core::ProgramCapabilityRequirementDTO& requirement,
    const RetainedBundle& retained_bundle,
    const ICatalogArtifactSource& catalog_artifact_store) {
  if (!verify_retained_bundle_identity(retained_bundle))
    fail("reference capability input is not a verified retained bundle");
  const auto& bundle = retained_bundle.value();
  const auto catalog = load_qualified_catalog_closure(bundle, catalog_artifact_store);
  const auto* definition = find_qualified_capability(
      catalog.capabilities, requirement.capability_id.value(),
      requirement.capability_definition_revision.value());
  if (!definition) {
    if (std::any_of(catalog.capabilities.begin(), catalog.capabilities.end(),
                    [&](const auto& value) {
                      return value.value().capability_id == requirement.capability_id;
                    }))
      fail("cell capability retained definition revision does not match");
    return false;
  }
  if (definition->value().scope != core::CapabilityScope::kCellGlobal)
    fail("cell capability selects a non-global retained definition");
  std::size_t count = 0U;
  for (const auto& provider : bundle.capability_provider_bindings) {
    const auto* target = std::get_if<core::CellGlobalCapabilityProviderTargetDTO>(&provider.target);
    if (provider.capability_id == requirement.capability_id &&
        provider.capability_definition_revision == requirement.capability_definition_revision &&
        provider.scope == core::CapabilityScope::kCellGlobal && target &&
        target->cell_id == bundle.cell_graph.cell_id) ++count;
  }
  if (count > 1U) fail("ambiguous retained cell capability providers");
  return count == 1U;
}


} // namespace manyforge::deployment
