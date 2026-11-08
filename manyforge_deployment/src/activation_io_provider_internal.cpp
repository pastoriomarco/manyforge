#include "activation_io_provider_internal.hpp"

#include <algorithm>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/types/capability_definition.hpp>
#include <manyforge_deployment/closed_schema.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <cmath>
#include <manyforge_core/types/typed_error.hpp>
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

// One closed-schema-qualified component type derived from its targets (an
// I/O device's `io` targets; none for a machine owner).
ActivationPreparedIoType
derived_type(const std::string &component_id, std::string_view tag,
             Array target_rows,
             const std::vector<QualifiedCapabilityDefinition> &selected_capabilities,
             Value config) {
  Value shape(Object{{"tags", Value(Array{Value(std::string(tag))})},
                     {"model_roles", Value(Array{})},
                     {"model_mounts", Value(Array{})},
                     {"attachment_interfaces", Value(Array{})},
                     {"command_targets", Value(std::move(target_rows))}});
  const auto shape_digest = content_identity::framed_sha256(
      "io-provider-component-shape", core::kComponentTypeApiVersion, shape);
  const core::ComponentTypeId id("manyforge." + std::string(tag) + "." +
                          shape_digest.substr(7U));
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
  return {component_id, std::move(type), bytes, std::move(config)};
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
          !names.insert(member(signal, "signal_name").as_string()).second)
        throw std::invalid_argument("I/O-only component has an invalid signal "
                                    "owner or name");
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
    result.push_back(derived_type(component.component_id, "io",
                                  std::move(target_rows),
                                  selected_capabilities, config));
  }
  return result;
}
} // namespace manyforge::deployment::detail

namespace manyforge::deployment::detail {
std::vector<ActivationPreparedIoType> prepare_activation_machine_types(
    const ActivationDeploymentIntent &intent,
    std::span<const ActivationComponentProfile> profiles) {
  if (intent.components.size() != profiles.size())
    throw std::invalid_argument("machine component profile join differs");
  std::vector<ActivationPreparedIoType> result;
  for (std::size_t index = 0; index != profiles.size(); ++index) {
    const auto &profile = profiles[index];
    if (profile.runtime_kind != ActivationRuntimeKind::kMachine)
      continue;
    const auto &component = intent.components[index];
    // The owner's compiled row.
    Object row{{"controllable_component_id", Value(component.component_id)},
               {"lane", Value(*profile.lane)}};
    if (profile.lane_row)
      row.emplace_back("machine_lane_row", Value(canonical_json(*profile.lane_row)));
    if (profile.owner_device_address_ref)
      row.emplace_back("device_address_ref", *profile.owner_device_address_ref);
    if (const auto &arm = profile.deactivated_arm) {
      Array joints;
      for (const auto &joint : arm->joint_names) joints.emplace_back(joint);
      row.emplace_back("arm_lane_row", Value(canonical_json(arm->lane_row)));
      row.emplace_back("arm_model_family_id", Value(arm->model_family_id));
      row.emplace_back("arm_description", Value(arm->description));
      row.emplace_back("arm_joint_names", Value(std::move(joints)));
    }
    result.push_back(derived_type(
        component.component_id, "machine", Array{}, {},
        Value(Object{{"machines", Value(Array{Value(std::move(row))})}})));
  }
  return result;
}
} // namespace manyforge::deployment::detail

namespace manyforge::deployment::detail {
namespace {

/// An absolute ROS name: `/`-separated tokens of `[A-Za-z_][A-Za-z0-9_]*`.
bool absolute_ros_name(std::string_view name) {
  if (name.size() < 2U || name.size() > 1024U || name.front() != '/' ||
      name.back() == '/')
    return false;
  bool token_start = true;
  for (const char c : name.substr(1U)) {
    if (c == '/') {
      if (token_start) return false;
      token_start = true;
    } else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' ||
               (!token_start && c >= '0' && c <= '9')) {
      token_start = false;
    } else {
      return false;
    }
  }
  return true;
}

/// The effective settings of one point-cloud source.
Value pointcloud_settings(const std::string &component_id, const Value &authored) {
  const auto invalid = [&](const std::string &problem) {
    return core::TypedError(
        "sensor_configuration_invalid",
        "Activation sensor: sensor_configuration_invalid: " + component_id +
            ": " + problem,
        {{"component_id", component_id}});
  };
  if (!authored.is_object())
    throw invalid("runtime_configuration must be an object");
  for (const auto &[name, unused] : authored.as_object()) {
    static_cast<void>(unused);
    if (name != "topic" && name != "source_frame_id" && name != "max_points" &&
        name != "minimum_period_s")
      throw invalid("unknown setting " + name);
  }
  const auto *topic = authored.find_object_member("topic");
  if (topic == nullptr || !topic->is_string() ||
      !absolute_ros_name(topic->as_string()))
    throw invalid("topic must be an absolute ROS topic name");
  const auto *frame = authored.find_object_member("source_frame_id");
  if (frame == nullptr || !frame->is_string() || frame->as_string().empty() ||
      frame->as_string().size() > 256U)
    throw invalid("source_frame_id must be a frame name");
  const auto *points = authored.find_object_member("max_points");
  const auto *point_count =
      points == nullptr ? nullptr : std::get_if<double>(&points->storage());
  if (point_count == nullptr || !(*point_count >= 1.0) ||
      *point_count > 200000.0 || std::floor(*point_count) != *point_count)
    throw invalid("max_points must be a whole number from 1 to 200000");
  const auto *period = authored.find_object_member("minimum_period_s");
  const auto *seconds =
      period == nullptr ? nullptr : std::get_if<double>(&period->storage());
  if (seconds == nullptr || !std::isfinite(*seconds) || *seconds < 0.0 ||
      *seconds > 10.0)
    throw invalid("minimum_period_s must be seconds from 0 to 10");
  return Value(Object{{"max_points", *points},
                      {"minimum_period_s", *period},
                      {"source_frame_id", *frame},
                      {"topic", *topic}});
}

} // namespace

std::vector<ActivationPreparedIoType> prepare_activation_sensor_types(
    const ActivationDeploymentIntent &intent,
    std::span<const ActivationComponentProfile> profiles) {
  if (intent.components.size() != profiles.size())
    throw std::invalid_argument("sensor component profile join differs");
  std::vector<ActivationPreparedIoType> result;
  for (std::size_t index = 0; index != profiles.size(); ++index) {
    const auto &profile = profiles[index];
    if (profile.runtime_kind != ActivationRuntimeKind::kSensor)
      continue;
    const auto &component = intent.components[index];
    const auto *sensor = profile.model_descriptor.find_object_member("sensor");
    if (sensor == nullptr || !sensor->is_string() ||
        sensor->as_string() != "pointcloud")
      throw core::TypedError(
          "sensor_configuration_invalid",
          "Activation sensor: sensor_configuration_invalid: " +
              component.component_id + ": model " +
              component.component_profile_id + " names no supported sensor",
          {{"component_id", component.component_id}});
    const auto settings = pointcloud_settings(
        component.component_id,
        member(component.configuration, "runtime_configuration"));
    // The model's one adapter row (a sensor has no lane).
    Object row{{"adapter_row", Value(canonical_json(member(
                                   profile.model_descriptor, "adapter_row")))},
               {"controllable_component_id", Value(component.component_id)},
               {"settings", Value(canonical_json(settings))}};
    result.push_back(derived_type(
        component.component_id, "sensor", Array{}, {},
        Value(Object{{"sensors", Value(Array{Value(std::move(row))})}})));
  }
  return result;
}

} // namespace manyforge::deployment::detail
