#include "ros2_control_execution_runtime_unit_internal.hpp"
#include <manyforge_deployment/execution_runtime_contract.hpp>
#include "native_execution_runtime_unit_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <utility>

#include "manyforge_deployment/artifact_codec.hpp"
#include "manyforge_deployment/closed_schema.hpp"
#include "manyforge_deployment/content_identity.hpp"
#include "manyforge_deployment/runtime_contract_registry.hpp"
#include "verified_retained_bundle_internal.hpp"

namespace manyforge::deployment::detail {
namespace {

[[nodiscard]] const CanonicalValue::Object &
require_object(const CanonicalValue &value, std::string_view field) {
  const auto *object = std::get_if<CanonicalValue::Object>(&value.storage());
  if (object == nullptr) {
    fail_retained_bundle(std::string(field) + " must be an object");
  }
  return *object;
}

[[nodiscard]] const CanonicalValue &
require_member(const CanonicalValue::Object &object, std::string_view name,
               std::string_view field) {
  const auto found =
      std::find_if(object.begin(), object.end(),
                   [&](const auto &member) { return member.first == name; });
  if (found == object.end()) {
    fail_retained_bundle(std::string(field) + " is missing " +
                         std::string(name));
  }
  return found->second;
}

[[nodiscard]] const std::string &require_string(const CanonicalValue &value,
                                                std::string_view field) {
  const auto *text = std::get_if<std::string>(&value.storage());
  if (text == nullptr) {
    fail_retained_bundle(std::string(field) + " must be a string");
  }
  return *text;
}

[[nodiscard]] CanonicalValue
json_value_to_canonical(const core::JsonValueDTO &value);

[[nodiscard]] CanonicalValue
json_object_to_canonical(const core::JsonObjectDTO &object) {
  CanonicalValue::Object result;
  result.reserve(object.size());
  for (const auto &[name, value] : object) {
    result.emplace_back(name, json_value_to_canonical(value));
  }
  return CanonicalValue(std::move(result));
}

[[nodiscard]] CanonicalValue
json_value_to_canonical(const core::JsonValueDTO &value) {
  return std::visit(
      [](const auto &current) -> CanonicalValue {
        using Value = std::decay_t<decltype(current)>;
        if constexpr (std::is_same_v<Value, std::nullptr_t>) {
          return CanonicalValue(nullptr);
        } else if constexpr (std::is_same_v<Value, bool> ||
                             std::is_same_v<Value, double> ||
                             std::is_same_v<Value, std::string>) {
          return CanonicalValue(current);
        } else if constexpr (std::is_same_v<Value,
                                            core::JsonValueDTO::Array>) {
          CanonicalValue::Array array;
          array.reserve(current.size());
          for (const auto &item : current) {
            array.push_back(json_value_to_canonical(item));
          }
          return CanonicalValue(std::move(array));
        } else {
          return json_object_to_canonical(current);
        }
      },
      value.value);
}

[[nodiscard]] core::JsonValueDTO
canonical_to_json_value(const CanonicalValue &value);

[[nodiscard]] core::JsonObjectDTO
canonical_to_json_object(const CanonicalValue &value, std::string_view field) {
  const auto &object = require_object(value, field);
  core::JsonObjectDTO result;
  for (const auto &[name, member] : object) {
    result.emplace(name, canonical_to_json_value(member));
  }
  return result;
}

[[nodiscard]] core::JsonValueDTO
canonical_to_json_value(const CanonicalValue &value) {
  return std::visit(
      [&value](const auto &current) -> core::JsonValueDTO {
        using Value = std::decay_t<decltype(current)>;
        if constexpr (std::is_same_v<Value, std::nullptr_t> ||
                      std::is_same_v<Value, bool> ||
                      std::is_same_v<Value, double> ||
                      std::is_same_v<Value, std::string>) {
          return core::JsonValueDTO{current};
        } else if constexpr (std::is_same_v<Value,
                                            CanonicalValue::Array>) {
          core::JsonValueDTO::Array array;
          array.reserve(current.size());
          for (const auto &item : current) {
            array.push_back(canonical_to_json_value(item));
          }
          return core::JsonValueDTO{std::move(array)};
        } else {
          return core::JsonValueDTO{canonical_to_json_object(
              value, "ros2_control_runtime_configuration")};
        }
      },
      value.storage());
}

[[nodiscard]] CanonicalValue require_qualified_runtime_configuration(
    const core::JsonObjectDTO &configuration) {
  const auto &registry = build_qualified_closed_schema_registry();
  const auto *schema = registry.find_qualified_value_schema(
      core::SchemaId(
          std::string(kRos2ControlExecutionRuntimeConfigurationSchemaId)),
      core::SchemaRevision(std::string(
          kRos2ControlExecutionRuntimeConfigurationSchemaRevision)));
  if (schema == nullptr) {
    fail_retained_bundle(
        "ros2_control execution configuration schema is absent from this "
        "build");
  }
  const CanonicalValue candidate = json_object_to_canonical(configuration);
  try {
    const auto normalized =
        validate_and_normalize_build_qualified_value(*schema, candidate);
    if (normalized.normalized_value() != candidate) {
      fail_retained_bundle("ros2_control execution configuration is not in "
                           "canonical normalized form");
    }
    return normalized.normalized_value();
  } catch (const ClosedSchemaError &error) {
    fail_retained_bundle(std::string("ros2_control execution configuration: ") +
                         error.what());
  } catch (const CanonicalValueError &error) {
    fail_retained_bundle(std::string("ros2_control execution configuration: ") +
                         error.what());
  }
}

[[nodiscard]] std::string contract_revision(std::string_view domain,
                                            std::string_view description) {
  std::string preimage(
      "manyforge.ros2-control-execution-runtime-contract.v1");
  preimage.push_back('\0');
  preimage.append(domain);
  preimage.push_back('\0');
  preimage.append(description);
  return sha256_digest(preimage);
}

[[nodiscard]] const core::RuntimeUnitDTO &
require_retained_runtime_unit(const core::RetainedBundleDTO &bundle,
                              const core::RuntimeUnitId &runtime_unit_id) {
  const auto found = std::find_if(
      bundle.cell_graph.runtime_units.begin(),
      bundle.cell_graph.runtime_units.end(), [&](const auto &unit) {
        return unit.runtime_unit_id == runtime_unit_id;
      });
  if (found == bundle.cell_graph.runtime_units.end()) {
    fail_retained_bundle(
        "ros2_control execution product has no exact retained graph subject");
  }
  return *found;
}

[[nodiscard]] const core::RetainedRuntimeUnitProductsDTO &
require_runtime_product_group(const core::RetainedBundleDTO &bundle,
                              const core::RuntimeUnitId &runtime_unit_id) {
  const auto found =
      std::find_if(bundle.compiled_products.runtime_unit_products.begin(),
                   bundle.compiled_products.runtime_unit_products.end(),
                   [&](const auto &group) {
                     return group.runtime_unit_id == runtime_unit_id;
                   });
  if (found == bundle.compiled_products.runtime_unit_products.end() ||
      found->products.size() != 1U) {
    fail_retained_bundle(
        "ros2_control execution product has no exact retained product group");
  }
  return *found;
}

[[nodiscard]] const core::RetainedArtifactRefDTO &
require_artifact(const core::RetainedProductDTO &product,
                 std::string_view role) {
  const core::RetainedArtifactRefDTO *match = nullptr;
  for (const auto &artifact : product.artifact_refs) {
    if (artifact.artifact_role_id.value() != role) {
      continue;
    }
    if (match != nullptr) {
      fail_retained_bundle(
          "ros2_control execution product repeats an artifact role");
    }
    match = &artifact;
  }
  if (match == nullptr) {
    fail_retained_bundle(
        "ros2_control execution product omits an artifact role");
  }
  return *match;
}

} // namespace

namespace {

CanonicalValue ros2_control_execution_runtime_infrastructure_projection(
    const core::RuntimeUnitDTO &unit) {
  const auto &liveness =
      execution_runtime_contract::manager_liveness_policy();
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(
           std::string(kRos2ControlExecutionRuntimeManifestApiVersion))},
      {"runtime_unit_id", CanonicalValue(unit.runtime_unit_id.value())},
      {"runtime_product_id",
       CanonicalValue(
           ros2_control_execution_runtime_product_id(unit.runtime_unit_id))},
      {"configuration", CanonicalValue(CanonicalValue::Object{})},
      {"manager_liveness_policy",
       CanonicalValue(CanonicalValue::Object{
           {"runtime_policy_id",
            CanonicalValue(liveness.runtime_policy_id.value())},
           {"runtime_policy_revision",
            CanonicalValue(liveness.runtime_policy_revision.value())},
           {"heartbeat_period_ns",
            CanonicalValue(static_cast<double>(liveness.heartbeat_period_ns))},
           {"expiry_timeout_ns",
            CanonicalValue(static_cast<double>(liveness.expiry_timeout_ns))},
           {"local_safe_state_contract_id",
            CanonicalValue(liveness.local_safe_state_contract_id.value())},
           {"local_safe_state_contract_revision",
            CanonicalValue(liveness.local_safe_state_contract_revision.value())},
       })},
      {"runtime_profile",
       CanonicalValue(CanonicalValue::Object{
           {"runtime_profile_id",
            CanonicalValue(unit.runtime_profile_id.value())},
           {"runtime_profile_revision",
            CanonicalValue(unit.runtime_profile_revision.value())},
           {"implementation_id",
            CanonicalValue(std::string(
                kRos2ControlExecutionRuntimeImplementationId))},
           {"implementation_revision",
            CanonicalValue(
                ros2_control_execution_runtime_implementation_revision())},
       })},
      {"process_adapter",
       CanonicalValue(CanonicalValue::Object{
           {"lifecycle_control", CanonicalValue("manager_managed")},
           {"process_adapter_contract_id",
            CanonicalValue(std::string(
                kRos2ControlExecutionRuntimeProcessAdapterContractId))},
           {"process_adapter_contract_revision",
            CanonicalValue(
                ros2_control_execution_runtime_process_adapter_revision())},
           {"retrieval_contract_id",
            CanonicalValue(std::string(
                kRos2ControlExecutionRuntimeRetrievalContractId))},
           {"retrieval_contract_revision",
            CanonicalValue(
                ros2_control_execution_runtime_retrieval_contract_revision())},
       })},
      {"runtime_role_mappings",
       CanonicalValue(CanonicalValue::Array{
           CanonicalValue(CanonicalValue::Object{
               {"runtime_role_id",
                CanonicalValue(
                    std::string(kRos2ControlExecutionRuntimeRoleId))},
               {"implementation_role_id",
                CanonicalValue(std::string(
                    kRos2ControlExecutionRuntimeImplementationId))},
           }),
       })},
      {"source_endpoint_mappings",
       CanonicalValue(CanonicalValue::Array{
           CanonicalValue(CanonicalValue::Object{
               {"source_endpoint_role_id",
                CanonicalValue(std::string(
                    kRos2ControlExecutionRuntimeLogicalSceneEndpointRoleId))},
               {"endpoint_contract_id",
                CanonicalValue(std::string(
                    kRos2ControlExecutionRuntimeLogicalSceneEndpointContractId))},
               {"endpoint_contract_revision",
                CanonicalValue(
                    ros2_control_execution_runtime_logical_scene_endpoint_revision())},
               {"transport", CanonicalValue("adapter_private")},
           }),
           CanonicalValue(CanonicalValue::Object{
               {"source_endpoint_role_id",
                CanonicalValue(std::string(
                    kRos2ControlExecutionRuntimeStatusEndpointRoleId))},
               {"endpoint_contract_id",
                CanonicalValue(std::string(
                    kRos2ControlExecutionRuntimeStatusEndpointContractId))},
               {"endpoint_contract_revision",
                CanonicalValue(
                    ros2_control_execution_runtime_status_endpoint_revision())},
               {"transport", CanonicalValue("adapter_private")},
           }),
           CanonicalValue(CanonicalValue::Object{
               {"source_endpoint_role_id",
                CanonicalValue(std::string(
                    kRos2ControlExecutionRuntimeProjectionEndpointRoleId))},
               {"endpoint_contract_id",
                CanonicalValue(std::string(
                    kRos2ControlExecutionRuntimeProjectionEndpointContractId))},
               {"endpoint_contract_revision",
                CanonicalValue(
                    ros2_control_execution_runtime_projection_endpoint_revision())},
               {"transport", CanonicalValue("adapter_private")},
           }),
       })},
      {"launchable_artifact",
       CanonicalValue(CanonicalValue::Object{
           {"artifact_role_id",
            CanonicalValue(std::string(
                kRos2ControlExecutionRuntimeLaunchDescriptorArtifactRole))},
           {"media_type",
            CanonicalValue(std::string(
                kRos2ControlExecutionRuntimeLaunchDescriptorMediaType))},
           {"content_digest",
            CanonicalValue(sha256_digest(
                ros2_control_execution_runtime_launch_descriptor()))},
       })},
      {"platform_constraints",
       CanonicalValue(CanonicalValue::Object{
           {"operating_system",
            CanonicalValue(std::string(
                kRos2ControlExecutionRuntimeOperatingSystem))},
           {"launch_environment_contract",
            CanonicalValue(std::string(
                kRos2ControlExecutionRuntimeLaunchEnvironmentContract))},
       })},
      {"runtime_contract_offer",
       runtime_contract_offer_projection(
           require_runtime_contract_for_unit(unit).offer())},
  });
}

} // namespace

CanonicalValue ros2_control_execution_runtime_manifest_projection(
    const core::RuntimeUnitDTO &unit) {
  static_cast<void>(require_qualified_runtime_configuration(unit.configuration));
  if (unit.runtime_profile_id.value() != kRos2ControlExecutionRuntimeProfileId ||
      unit.runtime_profile_revision.value() != ros2_control_execution_runtime_profile_revision() ||
      !unit.dependency_runtime_unit_ids.empty()) {
    fail_retained_bundle("runtime unit differs from the stable ROS execution profile");
  }
  return ros2_control_execution_runtime_infrastructure_projection(unit);
}

std::string ros2_control_execution_runtime_profile_document() {
  CanonicalValue identity(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.runtime_profile.v1")},
      {"runtime_profile_id",
       CanonicalValue(std::string(kRos2ControlExecutionRuntimeProfileId))},
      {"configuration_schema_ref",
       CanonicalValue(CanonicalValue::Object{
           {"schema_id",
            CanonicalValue(std::string(
                kRos2ControlExecutionRuntimeConfigurationSchemaId))},
           {"schema_revision",
            CanonicalValue(std::string(
                kRos2ControlExecutionRuntimeConfigurationSchemaRevision))},
       })},
      {"process_contract",
       CanonicalValue(CanonicalValue::Object{
           {"lifecycle_control", CanonicalValue("manager_managed")},
       })},
      {"supervision_contract",
       CanonicalValue(CanonicalValue::Object{
           {"status_endpoint_role_id",
            CanonicalValue(std::string(
                kRos2ControlExecutionRuntimeStatusEndpointRoleId))},
       })},
      {"offered_runtime_role_ids",
       CanonicalValue(CanonicalValue::Array{CanonicalValue(
           std::string(kRos2ControlExecutionRuntimeRoleId))})},
      {"source_endpoint_role_ids",
       CanonicalValue(CanonicalValue::Array{
           CanonicalValue(std::string(
               kRos2ControlExecutionRuntimeLogicalSceneEndpointRoleId)),
           CanonicalValue(std::string(
               kRos2ControlExecutionRuntimeProjectionEndpointRoleId)),
           CanonicalValue(std::string(
               kRos2ControlExecutionRuntimeStatusEndpointRoleId)),
       })},
  });
  std::string preimage("manyforge.content-identity.v1");
  preimage.push_back('\0');
  preimage.append("catalog-record-revision/runtime_profile");
  preimage.push_back('\0');
  preimage.append("manyforge.runtime_profile.v1");
  preimage.push_back('\0');
  preimage.append(canonical_json(identity));
  auto root = identity.as_object();
  root.emplace_back("runtime_profile_revision",
                    CanonicalValue(sha256_digest(preimage)));
  return canonical_json(CanonicalValue(std::move(root)));
}

std::string ros2_control_execution_runtime_profile_revision() {
  // A pure function of build constants: derive it once per process.
  static const std::string revision = [] {
    const auto root = parse_strict_json_model_yaml(
                          ros2_control_execution_runtime_profile_document())
                          .root();
    return require_string(
        require_member(require_object(root, "ros2_control_runtime_profile"),
                       "runtime_profile_revision",
                       "ros2_control_runtime_profile"),
        "ros2_control_runtime_profile.runtime_profile_revision");
  }();
  return revision;
}

void require_no_exclusive_ros_controller_topic_collision(
    const CanonicalValue::Array &controller_profiles) {
  // Existing lite6/ur10e profile facts: both use these global command/state
  // topics. Adding a profile with different topics must extend these facts.
  std::map<std::string_view, std::string> owners;
  for (const auto &row : controller_profiles) {
    const auto *isaac = row.find_object_member("isaac_profile");
    if (isaac == nullptr)
      continue;
    if (!isaac->is_string() ||
        (isaac->as_string() != "lite6" && isaac->as_string() != "ur10e")) {
      fail_retained_bundle("unsupported Isaac controller profile");
    }
    const auto *component = row.find_object_member("controllable_component_id");
    if (component == nullptr || !component->is_string()) {
      fail_retained_bundle(
          "ROS controller profile lacks its component identity");
    }
    for (const std::string_view topic :
         {"/isaac_joint_commands", "/isaac_joint_states"}) {
      const auto [owner, inserted] =
          owners.emplace(topic, component->as_string());
      if (!inserted) {
        fail_retained_bundle("exclusive Isaac topic collision between " +
                             owner->second + " and " + component->as_string() +
                             ": " + std::string(topic));
      }
    }
  }
}

bool ros_gripper_output_mode_is_valid(const std::string_view actuation_kind,
                                      const std::string_view output_mode) noexcept {
  return (actuation_kind == "pneumatic" &&
          (output_mode == "commanded" || output_mode == "none")) ||
         (actuation_kind == "position_commanded" && output_mode == "commanded");
}

bool ros_gripper_delivery_result_timeout_is_valid(
    const std::string_view actuation_kind, const double timeout_s) noexcept {
  return actuation_kind == "pneumatic" && std::isfinite(timeout_s) &&
         timeout_s > 0.0;
}

namespace {

// Shared by strict retained extraction and the pre-Draft required-row hook.
// This leaves the archival neutral twelve-field decoder unchanged.
std::optional<double> require_ros_gripper_delivery_result_timeout(
    const core::JsonObjectDTO &fields, const std::string_view actuation_kind) {
  const auto timeout = fields.find("delivery_result_timeout_s");
  const bool present = timeout != fields.end();
  if (fields.size() != (present ? 14U : 13U)) {
    fail_retained_bundle(
        "ros2_control runtime has an invalid gripper target configuration");
  }
  if (!present) {
    return std::nullopt;
  }
  const auto *seconds = std::get_if<double>(&timeout->second.value);
  if (seconds == nullptr ||
      !ros_gripper_delivery_result_timeout_is_valid(actuation_kind, *seconds)) {
    fail_retained_bundle(
        "ros2_control runtime has invalid pneumatic delivery result timeout");
  }
  return *seconds;
}

} // namespace

std::vector<ros2_control_execution_runtime_contract::Configuration::GripperTargetConfiguration>
decode_ros2_control_execution_gripper_target_configurations(
    const core::JsonValueDTO::Array &rows) {
  using Typed = ros2_control_execution_runtime_contract::Configuration::GripperTargetConfiguration;
  std::vector<Typed> result;
  result.reserve(rows.size());
  for (const auto &row : rows) {
    const auto *fields = std::get_if<core::JsonObjectDTO>(&row.value);
    if (fields == nullptr) {
      fail_retained_bundle("ros2_control runtime has an invalid gripper target configuration");
    }
    const auto output = fields->find("output_mode");
    const auto actuation = fields->find("actuation_kind");
    const auto *mode = output == fields->end() ? nullptr
                                              : std::get_if<std::string>(&output->second.value);
    const auto *kind = actuation == fields->end() ? nullptr
                                                : std::get_if<std::string>(&actuation->second.value);
    if (mode == nullptr || kind == nullptr || !ros_gripper_output_mode_is_valid(*kind, *mode)) {
      fail_retained_bundle("ros2_control runtime has invalid gripper output semantics");
    }
    const auto delivery_timeout =
        require_ros_gripper_delivery_result_timeout(*fields, *kind);
    auto common = *fields;
    common.erase("output_mode");
    common.erase("delivery_result_timeout_s");
    auto decoded = decode_execution_gripper_target_configurations(
        core::JsonValueDTO::Array{core::JsonValueDTO{std::move(common)}},
        "ros2_control runtime configuration");
    result.push_back(Typed{std::move(decoded.front()), *mode == "none"
                            ? core::GripperOutputMode::kNone
                            : core::GripperOutputMode::kCommanded, delivery_timeout});
  }
  std::sort(result.begin(), result.end(), [](const auto &left, const auto &right) {
    return std::tie(left.controllable_component_id, left.command_target_id) <
           std::tie(right.controllable_component_id, right.command_target_id);
  });
  if (std::adjacent_find(result.begin(), result.end(), [](const auto &left, const auto &right) {
        return left.controllable_component_id == right.controllable_component_id &&
               left.command_target_id == right.command_target_id;
      }) != result.end()) {
    fail_retained_bundle("ros2_control runtime repeats a gripper target configuration");
  }
  return result;
}

std::optional<core::GripperActuationKind> classify_ros_gripper_actuation_kind(
    const core::CellGraphDTO &graph, const core::ControlBindingDTO &binding,
    std::span<const CompositionControlBindingCoordinateMap> coordinate_maps) {
  const auto fail = [](std::string_view message) {
    fail_retained_bundle(message);
  };
  const auto exact = std::find_if(
      graph.control_bindings.begin(), graph.control_bindings.end(),
      [&](const auto &candidate) {
        return candidate.control_binding_id == binding.control_binding_id;
      });
  if (exact == graph.control_bindings.end() || *exact != binding ||
      std::count_if(graph.control_bindings.begin(),
                    graph.control_bindings.end(), [&](const auto &candidate) {
                      return candidate.control_binding_id ==
                             binding.control_binding_id;
                    }) != 1)
    fail("gripper actuation source has no exact unique graph binding");
  const auto command = std::find_if(
      graph.control_bindings.begin(), graph.control_bindings.end(),
      [&](const auto &candidate) {
        return candidate.controllable_component_id ==
                   binding.controllable_component_id &&
               candidate.command_target_id == binding.command_target_id &&
               candidate.capability_id.value() == "tool.gripper.command";
      });
  if (command == graph.control_bindings.end())
    return std::nullopt;
  const auto unit = std::find_if(
      graph.runtime_units.begin(), graph.runtime_units.end(),
      [&](const auto &candidate) {
        return candidate.runtime_unit_id == binding.provider_runtime_unit_id;
      });
  if (unit == graph.runtime_units.end() ||
      std::count_if(graph.runtime_units.begin(), graph.runtime_units.end(),
                    [&](const auto &candidate) {
                      return candidate.runtime_unit_id ==
                             binding.provider_runtime_unit_id;
                    }) != 1)
    fail("gripper actuation source has no exact runtime unit");
  if (unit->runtime_profile_id.value() != kRos2ControlExecutionRuntimeProfileId)
    return std::nullopt; // DX753: archival/native and other providers retain
                         // their floor.
  const auto component = std::find_if(
      graph.controllable_components.begin(),
      graph.controllable_components.end(), [&](const auto &candidate) {
        return candidate.controllable_component_id ==
               binding.controllable_component_id;
      });
  if (component == graph.controllable_components.end() ||
      std::find(component->runtime_unit_ids.begin(),
                component->runtime_unit_ids.end(),
                unit->runtime_unit_id) == component->runtime_unit_ids.end())
    fail("gripper actuation source does not host its exact component");
  const auto coordinates = std::find_if(
      coordinate_maps.begin(), coordinate_maps.end(),
      [&](const auto &candidate) {
        return candidate.control_binding_id == binding.control_binding_id;
      });
  if (coordinates == coordinate_maps.end() ||
      std::count_if(coordinate_maps.begin(), coordinate_maps.end(),
                    [&](const auto &candidate) {
                      return candidate.control_binding_id ==
                             binding.control_binding_id;
                    }) != 1 ||
      coordinates->controllable_component_id !=
          binding.controllable_component_id ||
      coordinates->command_target_id != binding.command_target_id)
    fail("gripper actuation source has no exact verified binding coordinate "
         "map");
  std::set<std::pair<core::ResourceRoleId, core::ResourceId>>
      expected_resources;
  for (const auto &resource : binding.resource_bindings)
    expected_resources.emplace(resource.resource_role_id, resource.resource_id);
  std::set<std::pair<core::ResourceRoleId, core::ResourceId>> mapped_resources;
  for (const auto &resource : coordinates->resources) {
    if (!mapped_resources
             .emplace(resource.resource_role_id, resource.resource_id)
             .second)
      fail("gripper actuation source repeats a coordinate resource");
    for (const auto &coordinate : resource.coordinates) {
      const bool owned_model = std::any_of(
          component->model_role_bindings.begin(),
          component->model_role_bindings.end(), [&](const auto &role) {
            return role.model_instance_id == coordinate.model_instance_id;
          });
      if (!owned_model ||
          std::none_of(graph.model_instances.begin(),
                       graph.model_instances.end(), [&](const auto &model) {
                         return model.model_instance_id ==
                                coordinate.model_instance_id;
                       }))
        fail("gripper actuation coordinate belongs to a foreign model "
             "occurrence");
    }
  }
  if (mapped_resources != expected_resources)
    fail("gripper actuation source coordinate resources differ from the "
         "binding");
  if (std::none_of(
          coordinates->resources.begin(), coordinates->resources.end(),
          [](const auto &resource) { return !resource.coordinates.empty(); }))
    return std::nullopt; // Preserve the existing component-local model
                         // boundary.
  const auto resources = [](const auto &selected) {
    std::set<core::ResourceId> result;
    for (const auto &resource : selected.resource_bindings) {
      if (resource.resource_id.empty() ||
          !result.insert(resource.resource_id).second)
        fail_retained_bundle(
            "gripper actuation source repeats or omits a resource");
    }
    return result;
  };
  if (command->provider_runtime_unit_id != binding.provider_runtime_unit_id ||
      command->provider_instance_id != binding.provider_instance_id ||
      command->provider_manifest_id != binding.provider_manifest_id ||
      command->provider_manifest_revision !=
          binding.provider_manifest_revision ||
      resources(binding).empty() || resources(*command) != resources(binding))
    fail("gripper actuation source differs from its exact command "
         "owner/resources");
  const auto found = unit->configuration.find("gripper_target_configurations");
  const auto *rows =
      found == unit->configuration.end()
          ? nullptr
          : std::get_if<core::JsonValueDTO::Array>(&found->second.value);
  if (rows == nullptr)
    fail("model-backed ROS gripper binding has no strict authored target "
         "configuration");
  const auto configurations =
      decode_ros2_control_execution_gripper_target_configurations(*rows);
  const ros2_control_execution_runtime_contract::Configuration::
      GripperTargetConfiguration *selected = nullptr;
  for (const auto &configuration : configurations) {
    const bool hosted = std::any_of(
        graph.control_bindings.begin(), graph.control_bindings.end(),
        [&](const auto &candidate) {
          return candidate.provider_runtime_unit_id == unit->runtime_unit_id &&
                 candidate.capability_id.value() == "tool.gripper.command" &&
                 candidate.controllable_component_id ==
                     configuration.controllable_component_id &&
                 candidate.command_target_id == configuration.command_target_id;
        });
    if (!hosted)
      fail("ROS gripper target configuration belongs to no hosted command "
           "binding");
    if (configuration.controllable_component_id ==
            binding.controllable_component_id &&
        configuration.command_target_id == binding.command_target_id) {
      if (selected != nullptr)
        fail("ROS gripper actuation source duplicates its target");
      selected = &configuration;
    }
  }
  if (selected == nullptr)
    fail("model-backed ROS gripper binding lacks one exact authored target "
         "configuration");
  if (selected->actuation_kind == "pneumatic")
    return core::GripperActuationKind::kPneumatic;
  if (selected->actuation_kind == "position_commanded")
    return core::GripperActuationKind::kPositionCommanded;
  fail_retained_bundle(
      "ROS gripper target configuration has unknown actuation semantics");
}

bool require_ros2_control_execution_runtime_required_rows(
    const core::CellGraphDTO &graph, const core::RuntimeUnitDTO &unit) {
  // This hook also runs while compiling/verifying a Draft, immediately after
  // schema normalization. Keep position-positive validation at that boundary
  // while allowing DX777's zero pneumatic dwell, without imposing unrelated
  // retained-decoder requirements on authoring.
  if (const auto found = unit.configuration.find("gripper_target_configurations");
      found != unit.configuration.end()) {
    const auto *rows = std::get_if<core::JsonValueDTO::Array>(&found->second.value);
    if (rows == nullptr) {
      fail_retained_bundle(
          "ros2_control runtime configuration has invalid gripper target configurations");
    }
    for (const auto &row : *rows) {
      const auto *fields = std::get_if<core::JsonObjectDTO>(&row.value);
      if (fields == nullptr) {
        fail_retained_bundle(
            "ros2_control runtime configuration has an invalid gripper target configuration");
      }
      const auto actuation = fields->find("actuation_kind");
      const auto duration = fields->find("command_duration_s");
      const auto *kind = actuation == fields->end()
                             ? nullptr
                             : std::get_if<std::string>(&actuation->second.value);
      const auto *seconds = duration == fields->end()
                                ? nullptr
                                : std::get_if<double>(&duration->second.value);
      const auto output = fields->find("output_mode");
      const auto *mode = output == fields->end() ? nullptr
                                                : std::get_if<std::string>(&output->second.value);
      if (kind == nullptr || seconds == nullptr || mode == nullptr ||
          !execution_gripper_command_duration_is_valid(*kind, *seconds) ||
          !ros_gripper_output_mode_is_valid(*kind, *mode)) {
        fail_retained_bundle(
            "ros2_control runtime configuration has invalid gripper target semantics");
      }
      (void)require_ros_gripper_delivery_result_timeout(*fields, *kind);
    }
  }
  const bool empty_host = std::none_of(
      graph.controllable_components.begin(), graph.controllable_components.end(),
      [&](const auto &component) {
        return std::find(component.runtime_unit_ids.begin(),
                         component.runtime_unit_ids.end(), unit.runtime_unit_id) !=
               component.runtime_unit_ids.end();
      });
  if (!empty_host) {
    const bool has_motion = std::any_of(
        graph.control_bindings.begin(), graph.control_bindings.end(),
        [&](const auto &binding) {
          return binding.provider_runtime_unit_id == unit.runtime_unit_id &&
                 binding.capability_id.value() == "execution.trajectory.execute";
        });
    for (const std::string_view field : {"controller_profiles",
                                         "initial_joint_positions_rad",
                                         "motion_target_limits"}) {
      const auto member = unit.configuration.find(field);
      const auto *rows = member == unit.configuration.end()
                             ? nullptr
                             : std::get_if<core::JsonValueDTO::Array>(&member->second.value);
      if (!has_motion && field != "controller_profiles") {
        if (rows == nullptr || !rows->empty()) {
          fail_retained_bundle("ros2_control runtime requires empty " +
                               std::string(field) +
                               " without a hosted motion target");
        }
        continue;
      }
      if (rows == nullptr || rows->empty()) {
        fail_retained_bundle("ros2_control runtime requires nonempty " +
                             std::string(field) + " for hosted components");
      }
    }
  }
  return empty_host;
}

bool ros2_control_execution_runtime_configuration_is_qualified(
    const core::JsonObjectDTO &configuration) noexcept {
  try {
    static_cast<void>(require_qualified_runtime_configuration(configuration));
    return true;
  } catch (...) {
    return false;
  }
}

std::string ros2_control_execution_runtime_implementation_revision() {
  return contract_revision(
      "runtime-implementation",
      "ros2-control-execution-host;retained-addressed-kernel;external-"
      "controller-observation-only;commanded-simulation-authority;"
      "jtc-follow-joint-trajectory;topic-gripper-transport;"
      "authored-target-tcp-linear-speed-maximum;"
      "planner-scene-projection-v1;logical-scene-services-v1;"
      "build-owned-candidate-worker;runtime-contract-offer-correlation-v2;"
      "retained-manager-liveness-receiver-v2;isolated-manager-liveness-transport-v1;"
      "fenced-initial-aggregate-installation-v1;managed-startup-without-home-motion-v1;"
      "component-owned-frame-prefix-v1;infrastructure-only-runtime-manifest-v1");
}

std::string ros2_control_execution_runtime_process_adapter_revision() {
  return contract_revision(
      "process-adapter",
      "manager-managed;installed-target;fixed-argv;direct-child-epoch;"
      "dual-inherited-seqpacket-channels;same-user-direct-parent;"
      "command-fenced-readiness;runtime-contract-offer-digest-echo;"
      "fixed-sibling-candidate-worker;isolated-manager-liveness-rpc-v1");
}

std::string ros2_control_execution_runtime_retrieval_contract_revision() {
  return contract_revision(
      "retrieval-contract",
      "retained-raw-artifact;digest-and-size-verified-launch-descriptor");
}

std::string ros2_control_execution_runtime_status_endpoint_revision() {
  return contract_revision(
      "status-endpoint",
      "adapter-private;native-execution-status-v2;direct-child-epoch;"
      "source-sequence;phase;health;product;configuration;roles;fence;"
      "runtime-contract-offer-digest");
}

std::string ros2_control_execution_runtime_projection_endpoint_revision() {
  return contract_revision(
      "projection-endpoint",
      "planner-scene-projection-rpc-v1;exact-retained-binding;bounded-"
      "framing;request-replay;health;stage;activate;retire;discard;rebuild;"
      "lease;sha256-content-identity");
}

std::string ros2_control_execution_runtime_logical_scene_endpoint_revision() {
  return contract_revision(
      "logical-scene-endpoint",
      "adapter-private;logical-scene-services-v1;reference;query;shape;"
      "compare-and-commit-edit;collision;trace;exact-revision-and-lease");
}

std::string ros2_control_execution_runtime_launch_descriptor() {
  return canonical_json(CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.installed_target_launch.v1")},
      {"implementation_id",
       CanonicalValue(
           std::string(kRos2ControlExecutionRuntimeImplementationId))},
      {"implementation_revision",
       CanonicalValue(
           ros2_control_execution_runtime_implementation_revision())},
      {"target_id",
       CanonicalValue(
           std::string(kRos2ControlExecutionRuntimeInstalledTargetId))},
  }));
}

std::string ros2_control_execution_runtime_product_id(
    const core::RuntimeUnitId &runtime_unit_id) {
  require_retained_portable_identity(runtime_unit_id.value(),
                                     "runtime_unit_id");
  std::string preimage("manyforge.runtime-unit-product-id.v1");
  preimage.push_back('\0');
  preimage.append(runtime_unit_id.value());
  return "runtime_unit_configuration_" + sha256_digest(preimage).substr(7U);
}

CanonicalValue parse_and_verify_ros2_control_execution_runtime_manifest(
    std::string_view canonical_bytes) {
  if (canonical_bytes.empty() ||
      canonical_bytes.size() >
          kMaxRos2ControlExecutionRuntimeManifestBytes) {
    fail_retained_bundle("ros2_control execution runtime manifest is empty or "
                         "exceeds its bound");
  }
  CanonicalValue root;
  try {
    root = parse_strict_json_model_yaml(canonical_bytes).root();
  } catch (const std::exception &error) {
    fail_retained_bundle(
        std::string("ros2_control execution runtime manifest is invalid: ") +
        error.what());
  }
  if (canonical_json(root) != canonical_bytes) {
    fail_retained_bundle(
        "ros2_control execution runtime manifest is not canonical JSON");
  }
  const auto &object =
      require_object(root, "ros2_control_execution_runtime_manifest");
  const auto &api = require_string(
      require_member(object, "api_version",
                     "ros2_control_execution_runtime_manifest"),
      "ros2_control_execution_runtime_manifest.api_version");
  const auto &runtime_unit_id = require_string(
      require_member(object, "runtime_unit_id",
                     "ros2_control_execution_runtime_manifest"),
      "ros2_control_execution_runtime_manifest.runtime_unit_id");
  require_retained_portable_identity(runtime_unit_id, "runtime_unit_id");
  const auto configuration = canonical_to_json_object(
      require_member(object, "configuration",
                     "ros2_control_execution_runtime_manifest"),
      "ros2_control_execution_runtime_manifest.configuration");
  if (!configuration.empty()) {
    fail_retained_bundle("ros2_control execution runtime manifest configuration "
                         "must be infrastructure-only");
  }
  const core::RuntimeUnitDTO expected{
      core::RuntimeUnitId(runtime_unit_id),
      core::RuntimeProfileId(
          std::string(kRos2ControlExecutionRuntimeProfileId)),
      core::RuntimeProfileRevision(
          ros2_control_execution_runtime_profile_revision()),
      {},
      configuration,
  };
  const auto embedded_offer = decode_runtime_contract_offer_projection(
      require_member(object, "runtime_contract_offer",
                     "ros2_control_execution_runtime_manifest"),
      "ros2_control_execution_runtime_manifest.runtime_contract_offer");
  if (embedded_offer != require_runtime_contract_for_unit(expected).offer()) {
    fail_retained_bundle(
        "ros2_control runtime embedded offer differs from package offer");
  }
  if (api != kRos2ControlExecutionRuntimeManifestApiVersion ||
      root != ros2_control_execution_runtime_infrastructure_projection(expected)) {
    fail_retained_bundle("ros2_control execution runtime manifest differs "
                         "from its closed contract");
  }
  return root;
}

void require_ros2_control_execution_runtime_product_matches_retained_bundle(
    std::string_view manifest_bytes,
    std::string_view launch_descriptor_bytes,
    const core::RetainedBundleDTO &retained_bundle,
    const core::RuntimeUnitId &runtime_unit_id) {
  const auto manifest =
      parse_and_verify_ros2_control_execution_runtime_manifest(manifest_bytes);
  if (launch_descriptor_bytes !=
          ros2_control_execution_runtime_launch_descriptor() ||
      launch_descriptor_bytes.size() >
          kMaxRos2ControlExecutionLaunchDescriptorBytes) {
    fail_retained_bundle("ros2_control execution launch descriptor differs "
                         "from the build-owned artifact");
  }
  const auto &unit =
      require_retained_runtime_unit(retained_bundle, runtime_unit_id);
  if (unit.runtime_profile_id.value() !=
          kRos2ControlExecutionRuntimeProfileId ||
      unit.runtime_profile_revision.value() !=
          ros2_control_execution_runtime_profile_revision() ||
      !unit.dependency_runtime_unit_ids.empty() ||
      !ros2_control_execution_runtime_configuration_is_qualified(
          unit.configuration) ||
      manifest != ros2_control_execution_runtime_manifest_projection(unit)) {
    fail_retained_bundle("ros2_control execution manifest differs from its "
                         "retained graph/profile");
  }
  static_cast<void>(require_ros2_control_execution_runtime_required_rows(
      retained_bundle.cell_graph, unit));
  const auto &group =
      require_runtime_product_group(retained_bundle, runtime_unit_id);
  const auto &product = group.products.front();
  if (product.product_id.value() !=
          ros2_control_execution_runtime_product_id(runtime_unit_id) ||
      product.product_kind !=
          core::RetainedProductKind::kRuntimeUnitConfiguration ||
      product.product_contract_id.value() !=
          kRos2ControlExecutionRuntimeProductContractId ||
      product.product_contract_revision !=
          ros2_control_execution_runtime_product_contract_revision() ||
      product.artifact_refs.size() != 2U) {
    fail_retained_bundle("ros2_control execution retained product has an "
                         "incoherent identity");
  }
  const auto &manifest_ref = require_artifact(
      product, kRos2ControlExecutionRuntimeManifestArtifactRole);
  const auto &launch_ref = require_artifact(
      product, kRos2ControlExecutionRuntimeLaunchDescriptorArtifactRole);
  if (manifest_ref.media_type !=
          kRos2ControlExecutionRuntimeManifestMediaType ||
      manifest_ref.content_digest.value() != sha256_digest(manifest_bytes) ||
      manifest_ref.byte_size != manifest_bytes.size() ||
      launch_ref.media_type !=
          kRos2ControlExecutionRuntimeLaunchDescriptorMediaType ||
      launch_ref.content_digest.value() != sha256_digest(launch_descriptor_bytes) ||
      launch_ref.byte_size != launch_descriptor_bytes.size()) {
    fail_retained_bundle("ros2_control execution product artifact table "
                         "differs from its bytes");
  }
  bool source_found = false;
  for (const auto &source : retained_bundle.provenance.source_records) {
    std::visit(
        [&](const auto &typed) {
          using Source = std::remove_cvref_t<decltype(typed)>;
          if constexpr (Source::kRecordKind ==
                        core::CatalogRecordKind::kRuntimeProfile) {
            if (typed.record_id == unit.runtime_profile_id &&
                typed.revision == unit.runtime_profile_revision) {
              if (source_found) {
                fail_retained_bundle("ros2_control execution runtime profile "
                                     "source is duplicated");
              }
              source_found = true;
            }
          }
        },
        source);
  }
  const auto output = std::find_if(
      retained_bundle.provenance.output_records.begin(),
      retained_bundle.provenance.output_records.end(),
      [&](const auto &row) { return row.product_id == product.product_id; });
  if (!source_found ||
      output == retained_bundle.provenance.output_records.end() ||
      output->product_kind != product.product_kind ||
      output->content_digest != retained_product_output_digest(product)) {
    fail_retained_bundle("ros2_control execution product lacks exact "
                         "source/output provenance");
  }
}

} // namespace manyforge::deployment::detail

namespace manyforge::deployment::ros2_control_execution_runtime_contract {
namespace {

[[noreturn]] void fail_fault_profile(const std::string_view reason) {
  throw RetainedBundleError(
      "ros2_control runtime fault Stop profile is invalid: " +
      std::string(reason));
}

[[nodiscard]] const core::JsonValueDTO *
fault_member(const core::JsonObjectDTO &row, const std::string_view name) {
  const auto found = row.find(name);
  return found == row.end() ? nullptr : &found->second;
}

[[nodiscard]] const std::string &
fault_text(const core::JsonObjectDTO &row, const std::string_view name) {
  const auto *value = fault_member(row, name);
  const auto *text =
      value == nullptr ? nullptr : std::get_if<std::string>(&value->value);
  if (text == nullptr) {
    fail_fault_profile(std::string(name) + " is absent");
  }
  return *text;
}

[[nodiscard]] std::optional<bool>
fault_boolean(const core::JsonObjectDTO &row, const std::string_view name) {
  const auto *value = fault_member(row, name);
  if (value == nullptr) {
    return std::nullopt;
  }
  const auto *flag = std::get_if<bool>(&value->value);
  if (flag == nullptr) {
    fail_fault_profile(std::string(name) + " is not boolean");
  }
  return *flag;
}

// Decodes `main`'s authored `default_fault_stop_profile` fields as `main`'s
// kernel-config parser did, without deriving anything from the operator
// profile. Omitted natural completion is normalized as every authored Stop
// profile is (true only for soft_on_path; `main` honoured it only there).
[[nodiscard]] Configuration::FaultStopProfile
decode_fault_stop_profile(const core::JsonValueDTO &encoded) {
  const auto *row = std::get_if<core::JsonObjectDTO>(&encoded.value);
  if (row == nullptr) {
    fail_fault_profile("row is not an object");
  }
  Configuration::FaultStopProfile result{
      core::ControllableComponentId(
          fault_text(*row, "controllable_component_id")),
      core::CommandTargetId(fault_text(*row, "command_target_id")),
      {}};
  auto &profile = result.profile;
  profile.profile_id = fault_text(*row, "profile_id");
  const auto &mode = fault_text(*row, "stop_mode");
  if (mode == "soft_on_path") {
    profile.stop_mode = core::MotionInterruptStopMode::kSoftOnPath;
  } else if (mode == "spring_back_hold") {
    profile.stop_mode = core::MotionInterruptStopMode::kSpringBackHold;
  } else if (mode == "hard_deceleration") {
    profile.stop_mode = core::MotionInterruptStopMode::kHardDeceleration;
  } else {
    fail_fault_profile("stop_mode is unsupported");
  }
  if (const auto *value = fault_member(*row, "deceleration_time_s")) {
    const auto *number = std::get_if<double>(&value->value);
    if (number == nullptr) {
      fail_fault_profile("deceleration_time_s is not a number");
    }
    profile.deceleration_time_s = *number;
  }
  if (const auto *value =
          fault_member(*row, "per_joint_max_deceleration_rad_s2")) {
    const auto *limits = std::get_if<core::JsonValueDTO::Array>(&value->value);
    if (limits == nullptr) {
      fail_fault_profile("per_joint_max_deceleration_rad_s2 is not an array");
    }
    for (const auto &limit : *limits) {
      const auto *joint_row = std::get_if<core::JsonObjectDTO>(&limit.value);
      const auto *magnitude =
          joint_row == nullptr
              ? nullptr
              : fault_member(*joint_row, "max_deceleration_rad_s2");
      const auto *number = magnitude == nullptr
                               ? nullptr
                               : std::get_if<double>(&magnitude->value);
      if (number == nullptr ||
          !profile.per_joint_max_deceleration
               .emplace(fault_text(*joint_row, "source_joint_name"),
                        core::CoordinateAccelerationDTO{
                            core::AngularAccelerationDTO{*number}})
               .second) {
        fail_fault_profile("per-joint deceleration rows are invalid");
      }
    }
  }
  const bool controlled =
      profile.stop_mode != core::MotionInterruptStopMode::kHardDeceleration;
  const auto natural_completion =
      fault_boolean(*row, "allow_natural_completion_on_short_path");
  if (!controlled && natural_completion.has_value()) {
    fail_fault_profile(
        "hard_deceleration forbids allow_natural_completion_on_short_path");
  }
  profile.allow_natural_completion_on_short_path =
      natural_completion.value_or(
          profile.stop_mode == core::MotionInterruptStopMode::kSoftOnPath);
  profile.require_backend_confirmation =
      fault_boolean(*row, "require_backend_confirmation").value_or(true);
  profile.jerk_limits_enabled = controlled;
  if (const auto error = core::motion_interrupt_stop_profile_field_error(profile);
      !error.empty()) {
    fail_fault_profile(error);
  }
  return result;
}

} // namespace

CanonicalValue empty_host_configuration(
    const std::optional<std::string> &isaac_world) {
  CanonicalValue::Object configuration{
      {"mode", CanonicalValue("commanded_sim")},
      {"planner_rng_seed", CanonicalValue(12345.0)},
      {"execution_tick_hz", CanonicalValue(100.0)},
      {"collision_monitor_period_ms", CanonicalValue(50.0)},
      {"initial_joint_positions_rad", CanonicalValue(CanonicalValue::Array{})},
      {"motion_target_limits", CanonicalValue(CanonicalValue::Array{})},
      {"controller_profiles", CanonicalValue(CanonicalValue::Array{})},
  };
  if (isaac_world.has_value()) {
    configuration.emplace_back("isaac_world", CanonicalValue(*isaac_world));
  }
  return CanonicalValue(std::move(configuration));
}

CanonicalValue empty_host_deployment(
    const core::CellId &cell_id, const CanonicalValue &runtime_profile_reference,
    std::string_view deployment_id, std::string_view name,
    const std::optional<std::string> &isaac_world) {
  using Object = CanonicalValue::Object;
  using Array = CanonicalValue::Array;
  const auto *profile = runtime_profile_reference.find_object_member("record_id");
  const auto *revision = runtime_profile_reference.find_object_member("record_revision");
  if (profile == nullptr || revision == nullptr || !profile->is_string() ||
      profile->as_string() != kRuntimeProfileId || !revision->is_string()) {
    throw RetainedBundleError("empty ROS Deployment requires its qualified runtime profile");
  }
  return CanonicalValue(Object{
      {"api_version", CanonicalValue("manyforge.deployment.v1")},
      {"deployment_artifact_id", CanonicalValue(std::string(deployment_id))},
      {"deployment_revision", CanonicalValue("sha256:" + std::string(64U, '0'))},
      {"cell_id", CanonicalValue(cell_id.value())},
      {"name", CanonicalValue(std::string(name))},
      {"catalogs", CanonicalValue(Object{{"records", CanonicalValue(Array{runtime_profile_reference})}})},
      {"graph", CanonicalValue(Object{
          {"runtime_units", CanonicalValue(Array{CanonicalValue(Object{
              {"runtime_unit_id", CanonicalValue(std::string(kCellHostRuntimeUnitId))},
              {"runtime_profile_id", *profile},
              {"runtime_profile_revision", *revision},
              {"dependency_runtime_unit_ids", CanonicalValue(Array{})},
              {"configuration", empty_host_configuration(isaac_world)},
          })})},
          {"model_instances", CanonicalValue(Array{})},
          {"controllable_components", CanonicalValue(Array{})},
          {"control_domains", CanonicalValue(Array{})},
          {"control_bindings", CanonicalValue(Array{})},
          {"component_attachments", CanonicalValue(Array{})},
      })},
      {"desired_control_configurations", CanonicalValue(Array{})},
      {"capability_provider_bindings", CanonicalValue(Array{})},
  });
}

Configuration
require_configuration(const RetainedBundle &retained_bundle,
                      const core::RuntimeUnitId &runtime_unit_id) {
  if (!verify_retained_bundle_identity(retained_bundle)) {
    throw RetainedBundleError(
        "ros2_control runtime configuration received an unverifiable bundle");
  }
  const auto &unit = detail::require_retained_runtime_unit(
      retained_bundle.value(), runtime_unit_id);
  if (unit.runtime_profile_id.value() !=
          detail::kRos2ControlExecutionRuntimeProfileId ||
      unit.runtime_profile_revision.value() !=
          detail::ros2_control_execution_runtime_profile_revision() ||
      !unit.dependency_runtime_unit_ids.empty() ||
      !detail::ros2_control_execution_runtime_configuration_is_qualified(
          unit.configuration)) {
    throw RetainedBundleError(
        "ros2_control runtime unit differs from the build-qualified profile");
  }

  const auto require_member =
      [&](const std::string_view name) -> const core::JsonValueDTO & {
    const auto found = unit.configuration.find(name);
    if (found == unit.configuration.end()) {
      throw RetainedBundleError(
          "ros2_control runtime configuration is missing " +
          std::string(name));
    }
    return found->second;
  };
  const auto require_number = [&](const std::string_view name) {
    const auto *number = std::get_if<double>(&require_member(name).value);
    if (number == nullptr || !std::isfinite(*number)) {
      throw RetainedBundleError(
          "ros2_control runtime configuration has invalid " +
          std::string(name));
    }
    return *number;
  };
  const auto require_uint32 = [&](const std::string_view name) {
    const double number = require_number(name);
    if (std::trunc(number) != number || number < 0.0 ||
        number >
            static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
      throw RetainedBundleError(
          "ros2_control runtime configuration has invalid " +
          std::string(name));
    }
    return static_cast<std::uint32_t>(number);
  };
  const auto optional_number = [&](const std::string_view name)
      -> std::optional<double> {
    const auto found = unit.configuration.find(name);
    if (found == unit.configuration.end()) {
      return std::nullopt;
    }
    const auto *number = std::get_if<double>(&found->second.value);
    if (number == nullptr || !std::isfinite(*number)) {
      throw RetainedBundleError(
          "ros2_control runtime configuration has invalid " +
          std::string(name));
    }
    return *number;
  };
  const auto optional_uint32 = [&](const std::string_view name)
      -> std::optional<std::uint32_t> {
    const auto number = optional_number(name);
    if (!number.has_value()) {
      return std::nullopt;
    }
    if (std::trunc(*number) != *number || *number < 0.0 ||
        *number >
            static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
      throw RetainedBundleError(
          "ros2_control runtime configuration has invalid " +
          std::string(name));
    }
    return static_cast<std::uint32_t>(*number);
  };

  const bool empty_host =
      detail::require_ros2_control_execution_runtime_required_rows(
          retained_bundle.value().cell_graph, unit);

  const auto *mode = std::get_if<std::string>(&require_member("mode").value);
  const auto *profiles = std::get_if<core::JsonValueDTO::Array>(
      &require_member("controller_profiles").value);
  const auto *positions = std::get_if<core::JsonValueDTO::Array>(
      &require_member("initial_joint_positions_rad").value);
  if (mode == nullptr || *mode != "commanded_sim" || profiles == nullptr ||
      positions == nullptr ||
      (empty_host && (!profiles->empty() || !positions->empty()))) {
    throw RetainedBundleError("ros2_control runtime configuration has an "
                              "unsupported mode, profile, or initial state");
  }

  const auto canonical_configuration =
      detail::json_object_to_canonical(unit.configuration);
  detail::require_no_exclusive_ros_controller_topic_collision(
      canonical_configuration.find_object_member("controller_profiles")
          ->as_array());
  Configuration result;
  if (const auto found = unit.configuration.find("isaac_world");
      found != unit.configuration.end()) {
    const auto *world = std::get_if<std::string>(&found->second.value);
    if (world == nullptr || world->empty()) {
      throw RetainedBundleError(
          "ros2_control isaac_world must be a portable identifier");
    }
    result.isaac_world = *world;
  }
  // Q68: one controller profile per hosted component, exactly one lane each.
  for (const auto &row : *profiles) {
    const auto *object = std::get_if<core::JsonObjectDTO>(&row.value);
    const auto text = [&](const std::string_view name)
        -> std::optional<std::string> {
      if (object == nullptr) return std::nullopt;
      const auto found = object->find(name);
      if (found == object->end()) return std::nullopt;
      const auto *value = std::get_if<std::string>(&found->second.value);
      if (value == nullptr || value->empty()) {
        throw RetainedBundleError(
            "ros2_control runtime configuration has an invalid controller "
            "profile");
      }
      return *value;
    };
    std::string frame_prefix;
    if (object != nullptr) {
      if (const auto found = object->find("frame_prefix"); found != object->end()) {
        const auto *value = std::get_if<std::string>(&found->second.value);
        if (value == nullptr) {
          throw RetainedBundleError("ros2_control frame_prefix must be a string");
        }
        frame_prefix = *value;
      }
    }
    const auto component = text("controllable_component_id");
    auto isaac = text("isaac_profile");
    auto mock = text("mock_profile");
    if (!component.has_value() || isaac.has_value() == mock.has_value() ||
        object->size() != (object->contains("frame_prefix") ? 3U : 2U) ||
        (isaac.has_value() && *isaac != "lite6" && *isaac != "ur10e") ||
        (mock.has_value() && *mock != "lite6_lite_gripper" &&
         *mock != "ur10e_robotiq_2f_140" && *mock != "ur5_roboplan" &&
         *mock != "ur5e_robotiq_2f_85")) {
      throw RetainedBundleError("ros2_control runtime configuration has an "
                                "unsupported mode, profile, or initial state");
    }
    result.controller_profiles.push_back(Configuration::ControllerProfile{
        core::ControllableComponentId(*component), std::move(isaac),
        std::move(mock), std::move(frame_prefix)});
  }
  if (!result.isaac_world.has_value() &&
      std::any_of(result.controller_profiles.begin(),
                  result.controller_profiles.end(), [](const auto &profile) {
                    return profile.isaac_profile.has_value();
                  })) {
    throw RetainedBundleError(
        "isaac_world is required when a component uses an Isaac profile");
  }
  std::sort(result.controller_profiles.begin(), result.controller_profiles.end(),
            [](const auto &left, const auto &right) {
              return left.controllable_component_id <
                     right.controllable_component_id;
            });
  const auto *motion_limits = std::get_if<core::JsonValueDTO::Array>(
      &require_member("motion_target_limits").value);
  if (motion_limits == nullptr ||
      (empty_host && !motion_limits->empty())) {
    throw RetainedBundleError(
        "ros2_control runtime requires motion target limits");
  }
  result.motion_target_limits =
      detail::decode_execution_motion_target_limits(*motion_limits, false);
  std::set<std::pair<core::ControllableComponentId, core::CommandTargetId>>
      expected;
  std::set<std::pair<core::ControllableComponentId, core::CommandTargetId>>
      expected_grippers;
  for (const auto &binding :
       retained_bundle.value().cell_graph.control_bindings) {
    if (binding.provider_runtime_unit_id == runtime_unit_id &&
        binding.capability_id.value() == "execution.trajectory.execute") {
      expected.emplace(binding.controllable_component_id,
                       binding.command_target_id);
    }
    if (binding.provider_runtime_unit_id == runtime_unit_id &&
        binding.capability_id.value() == "tool.gripper.command") {
      expected_grippers.emplace(binding.controllable_component_id,
                                binding.command_target_id);
    }
  }
  std::set<std::pair<core::ControllableComponentId, core::CommandTargetId>>
      configured;
  for (const auto &limit : result.motion_target_limits) {
    configured.emplace(limit.controllable_component_id,
                       limit.command_target_id);
  }
  if (configured != expected) {
    throw RetainedBundleError(
        "ros2_control runtime must contain exactly one TCP-speed "
        "maximum for every motion target and no unknown target");
  }
  std::set<core::ControllableComponentId> hosted_components;
  for (const auto &[component, target] : expected) {
    static_cast<void>(target);
    hosted_components.insert(component);
  }
  for (const auto &[component, target] : expected_grippers) {
    static_cast<void>(target);
    hosted_components.insert(component);
  }
  std::set<core::ControllableComponentId> profiled_components;
  for (const auto &profile : result.controller_profiles) {
    if (!profiled_components.insert(profile.controllable_component_id).second) {
      throw RetainedBundleError(
          "ros2_control runtime repeats a component controller profile");
    }
  }
  if (profiled_components != hosted_components) {
    throw RetainedBundleError(
        "ros2_control runtime must select exactly one controller profile for "
        "every hosted component and no unknown component");
  }
  if (const auto found =
          unit.configuration.find("gripper_target_configurations");
      found != unit.configuration.end()) {
    const auto *rows =
        std::get_if<core::JsonValueDTO::Array>(&found->second.value);
    if (rows == nullptr) {
      throw RetainedBundleError(
          "ros2_control runtime configuration has invalid gripper target "
          "configurations");
    }
    result.gripper_target_configurations =
        detail::decode_ros2_control_execution_gripper_target_configurations(*rows);
    for (const auto &gripper : result.gripper_target_configurations) {
      const bool hosted = std::any_of(
          retained_bundle.value().cell_graph.control_bindings.begin(),
          retained_bundle.value().cell_graph.control_bindings.end(),
          [&](const auto &binding) {
            return binding.provider_runtime_unit_id == runtime_unit_id &&
                   binding.capability_id.value() == "tool.gripper.command" &&
                   binding.controllable_component_id ==
                       gripper.controllable_component_id &&
                   binding.command_target_id == gripper.command_target_id;
          });
      if (!hosted) {
        throw RetainedBundleError(
            "ros2_control runtime gripper configuration names a target this "
            "unit does not host");
      }
    }
  }
  if (!empty_host && expected.empty()) {
    std::set<std::pair<core::ControllableComponentId, core::CommandTargetId>>
        configured_grippers;
    for (const auto &gripper : result.gripper_target_configurations) {
      configured_grippers.emplace(gripper.controllable_component_id,
                                  gripper.command_target_id);
    }
    if (configured_grippers != expected_grippers) {
      throw RetainedBundleError(
          "ros2_control runtime without motion must contain exactly one "
          "configuration for every gripper target and no unknown target");
    }
  }
  if (const auto found = unit.configuration.find("default_fault_stop_profiles");
      found != unit.configuration.end()) {
    const auto *rows = std::get_if<core::JsonValueDTO::Array>(&found->second.value);
    if (rows == nullptr) {
      throw RetainedBundleError(
          "ros2_control runtime has invalid default_fault_stop_profiles");
    }
    for (const auto &row : *rows) {
      auto decoded = decode_fault_stop_profile(row);
      if (!expected.contains({decoded.controllable_component_id,
                              decoded.command_target_id})) {
        throw RetainedBundleError(
            "ros2_control runtime fault Stop profile names no hosted motion "
            "target");
      }
      result.default_fault_stop_profiles.push_back(std::move(decoded));
    }
  }
  result.planner_rng_seed = require_uint32("planner_rng_seed");
  result.execution_tick_hz = require_uint32("execution_tick_hz");
  result.collision_monitor_period_ms =
      require_number("collision_monitor_period_ms");
  result.runtime_collision_immediate_query_budget_ms =
      optional_number("runtime_collision_immediate_query_budget_ms");
  result.runtime_collision_lookahead_query_budget_ms =
      optional_number("runtime_collision_lookahead_query_budget_ms");
  result.runtime_collision_lookahead_cycle_budget_ms =
      optional_number("runtime_collision_lookahead_cycle_budget_ms");
  result.runtime_collision_lookahead_s =
      optional_number("runtime_collision_lookahead_s");
  result.runtime_collision_lookahead_max_samples =
      optional_uint32("runtime_collision_lookahead_max_samples");
  result.runtime_collision_lookahead_workers =
      optional_uint32("runtime_collision_lookahead_workers");
  result.runtime_dynamic_body_max_age_cap_ms =
      optional_number("runtime_dynamic_body_max_age_cap_ms");
  result.validation_collision_workers =
      optional_uint32("validation_collision_workers");
  result.initial_joint_positions_rad.reserve(positions->size());
  for (const auto &position : *positions) {
    const auto *number = std::get_if<double>(&position.value);
    if (number == nullptr || !std::isfinite(*number)) {
      throw RetainedBundleError(
          "ros2_control runtime configuration has an invalid initial joint "
          "state");
    }
    result.initial_joint_positions_rad.push_back(*number);
  }
  if (result.execution_tick_hz == 0U ||
      !(result.collision_monitor_period_ms > 0.0) ||
      !(result.runtime_collision_immediate_query_budget_ms.value_or(50.0) <=
        result.collision_monitor_period_ms) ||
      !(result.runtime_collision_lookahead_query_budget_ms.value_or(75.0) <=
        result.runtime_collision_lookahead_cycle_budget_ms.value_or(200.0))) {
    throw RetainedBundleError(
        "ros2_control runtime configuration has invalid collision cadences");
  }
  return result;
}

Configuration require_component_configuration(
    const RetainedBundle &bundle, const core::RuntimeUnitId &runtime_unit_id,
    const core::ControllableComponentId &component_id) {
  auto result = require_configuration(bundle, runtime_unit_id);
  // Reuse the native exact binding-resource ownership and aggregate ordering.
  execution_runtime_contract::Configuration motion;
  motion.initial_joint_positions_rad = result.initial_joint_positions_rad;
  motion.motion_target_limits = result.motion_target_limits;
  for (const auto &gripper : result.gripper_target_configurations) {
    motion.gripper_target_configurations.push_back(
        static_cast<const execution_runtime_contract::Configuration::GripperTargetConfiguration &>(gripper));
  }
  motion = detail::select_native_component_configuration(
      std::move(motion), bundle.value().cell_graph, runtime_unit_id, component_id);
  result.initial_joint_positions_rad = std::move(motion.initial_joint_positions_rad);
  result.motion_target_limits = std::move(motion.motion_target_limits);
  // The neutral selector verifies ownership/order; preserve the typed output
  // choice on the ROS rows instead of round-tripping through its base payload.
  std::erase_if(result.gripper_target_configurations, [&](const auto &row) {
    return row.controllable_component_id != component_id;
  });
  const auto unrelated = [&](const auto &row) {
    return row.controllable_component_id != component_id;
  };
  std::erase_if(result.controller_profiles, unrelated);
  std::erase_if(result.default_fault_stop_profiles, unrelated);
  if (std::none_of(result.controller_profiles.begin(),
                   result.controller_profiles.end(), [](const auto &profile) {
                     return profile.isaac_profile.has_value();
                   })) {
    result.isaac_world.reset();
  }
  return result;
}

CanonicalValue component_configuration_projection(
    const RetainedBundle &bundle, const core::RuntimeUnitId &runtime_unit_id,
    const core::ControllableComponentId &component_id) {
  const auto selected = require_component_configuration(bundle, runtime_unit_id, component_id);
  const auto *graph = bundle.normalized_root().find_object_member("cell_graph");
  const auto *units = graph == nullptr ? nullptr : graph->find_object_member("runtime_units");
  if (units == nullptr || !units->is_array()) {
    throw RetainedBundleError("ROS component configuration lacks its verified graph");
  }
  for (const auto &unit : units->as_array()) {
    const auto *id = unit.find_object_member("runtime_unit_id");
    if (id == nullptr || !id->is_string() || id->as_string() != runtime_unit_id.value()) continue;
    const auto *configuration = unit.find_object_member("configuration");
    if (configuration == nullptr || !configuration->is_object()) {
      throw RetainedBundleError("ROS component configuration lacks its verified settings");
    }
    auto result = configuration->as_object();
    if (!selected.isaac_world.has_value()) {
      std::erase_if(result,
                    [](const auto &row) { return row.first == "isaac_world"; });
    }
    for (auto &[name, value] : result) {
      if (name == "initial_joint_positions_rad") {
        CanonicalValue::Array positions;
        for (const auto position : selected.initial_joint_positions_rad) positions.emplace_back(position);
        value = CanonicalValue(std::move(positions));
      } else if (name == "controller_profiles" || name == "motion_target_limits" ||
                 name == "default_fault_stop_profiles" || name == "gripper_target_configurations") {
        CanonicalValue::Array rows;
        for (const auto &row : value.as_array()) {
          const auto *owner = row.find_object_member("controllable_component_id");
          if (owner == nullptr || !owner->is_string()) {
            throw RetainedBundleError("ROS target configuration has no component owner");
          }
          if (owner->as_string() == component_id.value()) rows.push_back(row);
        }
        value = CanonicalValue(std::move(rows));
      }
    }
    return CanonicalValue(std::move(result));
  }
  throw RetainedBundleError("ROS component configuration has no runtime projection");
}

const Configuration::ControllerProfile *
find_controller_profile(const Configuration &configuration,
                        const core::ControllableComponentId &component_id) {
  const auto found = std::find_if(
      configuration.controller_profiles.begin(),
      configuration.controller_profiles.end(), [&](const auto &profile) {
        return profile.controllable_component_id == component_id;
      });
  return found == configuration.controller_profiles.end() ? nullptr : &*found;
}

std::string implementation_revision() {
  return detail::ros2_control_execution_runtime_implementation_revision();
}

} // namespace manyforge::deployment::ros2_control_execution_runtime_contract
