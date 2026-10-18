#include "activation_io_provider_internal.hpp"

#include <algorithm>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/types/capability_definition.hpp>
#include <manyforge_deployment/closed_schema.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <map>
#include <set>
#include <stdexcept>

#include "content_identity_internal.hpp"

namespace manyforge::deployment::detail {
namespace {
using Value = CanonicalValue;
using Object = Value::Object;
using Array = Value::Array;
constexpr auto kIoConfigurationSchema =
    "manyforge.value_schema.runtime.io_provider_execution.v1";

const Value &member(const Value &value, std::string_view name) {
  const auto *result = value.find_object_member(name);
  if (!result)
    throw std::invalid_argument("I/O configuration missing " +
                                std::string(name));
  return *result;
}

Value with(Value value, std::string_view field, Value selected) {
  auto object = value.as_object();
  const auto found =
      std::find_if(object.begin(), object.end(),
                   [&](const auto &row) { return row.first == field; });
  if (found == object.end())
    object.emplace_back(field, std::move(selected));
  else
    found->second = std::move(selected);
  return Value(std::move(object));
}

Value normalize_configuration(const Value &value) {
  const auto &registry = build_qualified_closed_schema_registry();
  const BuildQualifiedValueSchemaEntry *selected = nullptr;
  for (const auto &entry : registry.value_schemas()) {
    if (entry.schema_id().value() != kIoConfigurationSchema)
      continue;
    if (selected)
      throw std::logic_error("I/O configuration schema identity is ambiguous");
    selected = &entry;
  }
  if (!selected)
    throw std::invalid_argument(
        "I/O component configuration schema is not qualified by this build");
  return validate_and_normalize_build_qualified_value(*selected, value)
      .normalized_value();
}

const QualifiedCapabilityDefinition &
capability(std::string_view name,
           std::span<const QualifiedCapabilityDefinition> capabilities) {
  const auto *builtin = core::find_builtin_capability_definition(name);
  if (!builtin)
    throw std::invalid_argument(
        "I/O capability is not qualified by this build");
  const QualifiedCapabilityDefinition *selected = nullptr;
  for (const auto &candidate : capabilities) {
    if (candidate.value().capability_id.value() != name ||
        candidate.value().capability_definition_revision.value() !=
            builtin->capability_definition_revision)
      continue;
    if (selected)
      throw std::invalid_argument("I/O capability closure is ambiguous");
    selected = &candidate;
  }
  if (!selected)
    throw std::invalid_argument(
        "I/O capability missing from exact source catalog");
  return *selected;
}
} // namespace

std::vector<ActivationPreparedIoType> prepare_activation_io_types(
    const ActivationDeploymentIntent &intent,
    std::span<const ActivationComponentProfile> profiles,
    std::span<const QualifiedCapabilityDefinition> capabilities) {
  if (intent.components.size() != profiles.size())
    throw std::invalid_argument("I/O component profile join differs");
  std::vector<ActivationPreparedIoType> result;
  for (std::size_t index = 0; index != profiles.size(); ++index) {
    const auto &profile = profiles[index];
    const auto &component = intent.components[index];
    if (component.component_id != profile.component_id)
      throw std::invalid_argument("I/O component profile ordering differs");
    if (profile.runtime_kind != ActivationRuntimeKind::kIoProvider)
      continue;
    const auto config = normalize_configuration(
        member(component.configuration, "runtime_configuration"));
    const auto &devices = member(config, "io_provider_devices").as_array();
    const auto &signals = member(config, "io_signals").as_array();
    if (devices.size() != 1U || signals.empty() ||
        member(devices.front(), "controllable_component_id").as_string() !=
            component.component_id)
      throw std::invalid_argument(
          "I/O device configuration does not name its actual component");
    struct Target {
      std::set<std::string> roles, capabilities;
    };
    std::map<std::string, Target> targets;
    std::set<std::string> names;
    std::set<std::string> required;
    for (const auto &signal : signals) {
      if (member(signal, "controllable_component_id").as_string() !=
              component.component_id ||
          !names.insert(member(signal, "signal_name").as_string()).second ||
          member(member(signal, "provider"), "profile").as_string() ==
              "xarm_controller_input")
        throw std::invalid_argument("I/O-only component has an invalid signal "
                                    "owner, name or arm provider");
      auto &target = targets[member(signal, "command_target_id").as_string()];
      if (!target.roles.insert(member(signal, "resource_role_id").as_string())
               .second)
        throw std::invalid_argument("I/O target repeats a resource role");
      if (member(signal, "direction").as_string() == "input") {
        target.capabilities.insert("io.gpio.get_input");
        target.capabilities.insert("io.gpio.wait_for_input");
      } else {
        target.capabilities.insert("io.gpio.set_output");
      }
      required.insert(target.capabilities.begin(), target.capabilities.end());
    }
    std::vector<QualifiedCapabilityDefinition> selected_capabilities;
    for (const auto &name : required)
      selected_capabilities.push_back(capability(name, capabilities));
    Array target_rows;
    for (const auto &[id, target] : targets) {
      Array roles, offers;
      for (const auto &role : target.roles)
        roles.emplace_back(Object{{"resource_role_id", Value(role)},
                                  {"source_kind", Value("component_local")}});
      for (const auto &name : target.capabilities) {
        const auto &definition =
            capability(name, selected_capabilities).value();
        offers.emplace_back(
            Object{{"capability_id", Value(definition.capability_id.value())},
                   {"capability_definition_revision",
                    Value(definition.capability_definition_revision.value())}});
      }
      target_rows.emplace_back(
          Object{{"command_target_id", Value(id)},
                 {"model_group_roles", Value(Array{})},
                 {"frame_roles", Value(Array{})},
                 {"collision_body_roles", Value(Array{})},
                 {"resource_roles", Value(std::move(roles))},
                 {"capabilities", Value(std::move(offers))}});
    }
    Value shape(Object{{"tags", Value(Array{Value("io")})},
                       {"model_roles", Value(Array{})},
                       {"model_mounts", Value(Array{})},
                       {"attachment_interfaces", Value(Array{})},
                       {"command_targets", Value(std::move(target_rows))}});
    const auto shape_digest = content_identity::framed_sha256(
        "io-provider-component-shape", core::kComponentTypeApiVersion, shape);
    const core::ComponentTypeId id("manyforge.io." + shape_digest.substr(7U));
    auto document = with(
        with(with(shape, "api_version",
                  Value(std::string(core::kComponentTypeApiVersion))),
             "component_type_id", Value(id.value())),
        "component_type_revision", Value("sha256:" + std::string(64, '0')));
    const auto *schema =
        build_qualified_closed_schema_registry().find_catalog_schema_family(
            core::CatalogRecordKind::kComponentType);
    if (!schema || !schema->compiled_schema())
      throw std::logic_error("component type schema is unavailable");
    const auto normalized =
        validate_and_normalize_unqualified_artifact_candidate(
            *schema->compiled_schema(),
            parse_strict_json_model_yaml(canonical_json(document)))
            .normalized_root();
    const auto revision = compute_content_identity(
        normalized, {catalog_record_identity_domain("component_type"),
                     std::string(core::kComponentTypeApiVersion),
                     "component_type_revision"});
    document = with(normalized, "component_type_revision", Value(revision));
    const auto bytes = canonical_json(document);
    const core::ContentDigest digest(sha256_digest(bytes));
    const core::ContentAddressedStoreKey key("sha256/" +
                                             digest.value().substr(7U));
    const core::ComponentTypeCatalogRecordRefDTO reference{
        id,
        core::ComponentTypeRevision(revision),
        schema->schema_id(),
        schema->compiled_schema()->schema_revision(),
        digest,
        key};
    auto type = resolve_and_qualify_component_type(reference, key, bytes,
                                                   selected_capabilities);
    result.push_back({component.component_id, std::move(type), bytes, config});
  }
  return result;
}
} // namespace manyforge::deployment::detail
