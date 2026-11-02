#include <manyforge_deployment/io_signal_catalog.hpp>
#include "ros2_control_execution_runtime_unit_internal.hpp"
#include <manyforge_deployment/execution_runtime_contract.hpp>
#include "execution_host_unit_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
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
    const CanonicalValue::Array &controller_profiles,
    const CanonicalValue::Array &motion_target_limits) {
  // Every Isaac arm uses these global command/state topics. Merge 6: only an
  // arm owns them; a tool (no motion target) declares its own joints from
  // the robot's shared Isaac joint states.
  std::set<std::string> arms;
  for (const auto &row : motion_target_limits) {
    if (const auto *component = row.find_object_member("controllable_component_id");
        component != nullptr && component->is_string())
      arms.insert(component->as_string());
  }
  std::map<std::string_view, std::string> owners;
  for (const auto &row : controller_profiles) {
    const auto *lane = row.find_object_member("lane");
    if (lane == nullptr || !lane->is_string() || lane->as_string() != "isaac")
      continue;
    const auto *component = row.find_object_member("controllable_component_id");
    if (component == nullptr || !component->is_string()) {
      fail_retained_bundle(
          "ROS controller profile lacks its component identity");
    }
    if (!arms.contains(component->as_string()))
      continue;
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
  if (fields.size() != 13U + static_cast<std::size_t>(present) +
                           static_cast<std::size_t>(fields.contains("pneumatic_io")) +
                           static_cast<std::size_t>(fields.contains("grasp_confirmation")) +
                           static_cast<std::size_t>(fields.contains("stop_finish_tolerance"))) {
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

using PneumaticIo = ros2_control_execution_runtime_contract::Configuration::PneumaticIo;

[[nodiscard]] core::IoSignalId decode_pneumatic_signal(const core::JsonValueDTO &value) {
  const auto *fields = std::get_if<core::JsonObjectDTO>(&value.value);
  if (fields == nullptr || fields->size() != 2U ||
      !fields->contains("controllable_component_id") || !fields->contains("signal_name"))
    fail_retained_bundle("ros2_control runtime has an invalid pneumatic signal reference");
  const auto *component = std::get_if<std::string>(&fields->at("controllable_component_id").value);
  const auto *name = std::get_if<std::string>(&fields->at("signal_name").value);
  if (component == nullptr || component->empty() || name == nullptr || name->empty())
    fail_retained_bundle("ros2_control runtime has an invalid pneumatic signal reference");
  return {core::ControllableComponentId(*component), *name};
}

[[nodiscard]] std::optional<PneumaticIo> decode_pneumatic_io(
    const core::JsonObjectDTO &fields, std::string_view actuation, std::string_view output) {
  const auto found = fields.find("pneumatic_io");
  if (found == fields.end()) return std::nullopt;
  const auto *io = std::get_if<core::JsonObjectDTO>(&found->second.value);
  if (actuation != "pneumatic" || output != "commanded" || io == nullptr ||
      (io->size() != 1U && io->size() != 2U) || !io->contains("outputs") ||
      (io->size() == 2U && !io->contains("inputs")))
    fail_retained_bundle("pneumatic_io requires a commanded pneumatic gripper");
  const auto *outputs = std::get_if<core::JsonValueDTO::Array>(&io->at("outputs").value);
  if (outputs == nullptr || outputs->empty() || outputs->size() > 2U)
    fail_retained_bundle("pneumatic_io requires one or two valve outputs");
  PneumaticIo result;
  for (const auto &row : *outputs) {
    const auto *entry = std::get_if<core::JsonObjectDTO>(&row.value);
    if (entry == nullptr || entry->size() != 3U || !entry->contains("signal") ||
        !entry->contains("open_value") || !entry->contains("close_value"))
      fail_retained_bundle("pneumatic_io has an invalid valve output");
    const auto *open = std::get_if<bool>(&entry->at("open_value").value);
    const auto *close = std::get_if<bool>(&entry->at("close_value").value);
    if (open == nullptr || close == nullptr)
      fail_retained_bundle("pneumatic_io has invalid valve output levels");
    result.outputs.push_back({decode_pneumatic_signal(entry->at("signal")), *open, *close});
  }
  std::sort(result.outputs.begin(), result.outputs.end(), [](const auto &left, const auto &right) {
    return left.signal < right.signal;
  });
  if (std::adjacent_find(result.outputs.begin(), result.outputs.end(),
                         [](const auto &left, const auto &right) {
                           return left.signal == right.signal;
                         }) != result.outputs.end())
    fail_retained_bundle("pneumatic_io repeats a valve output");
  if (const auto inputs = io->find("inputs"); inputs != io->end()) {
    const auto *input_rows = std::get_if<core::JsonObjectDTO>(&inputs->second.value);
    if (input_rows == nullptr || input_rows->size() > 4U)
      fail_retained_bundle("pneumatic_io has invalid inputs");
    for (const auto &[name, value] : *input_rows) {
      if (name != "open" && name != "closed" && name != "pressure" &&
          name != "workpiece_grasped")
        fail_retained_bundle("pneumatic_io has an unknown input");
      const auto *entry = std::get_if<core::JsonObjectDTO>(&value.value);
      if (entry == nullptr || entry->size() != 1U || !entry->contains("signal"))
        fail_retained_bundle("pneumatic_io has an invalid input reference");
      result.inputs.emplace(name, decode_pneumatic_signal(entry->at("signal")));
    }
  }
  return result;
}

using GraspConfirmation =
    ros2_control_execution_runtime_contract::Configuration::GraspConfirmation;

// Opt-in grasp confirmation: `{policy: required | not_required,
// stall_window?: {min_position, max_position}}`. The window is position-only;
// `required` needs its evidence source (window, or the pneumatic
// `workpiece_grasped` input).
[[nodiscard]] std::optional<GraspConfirmation> decode_grasp_confirmation(
    const core::JsonObjectDTO &fields, std::string_view actuation,
    const std::optional<PneumaticIo> &pneumatic_io) {
  const auto found = fields.find("grasp_confirmation");
  if (found == fields.end()) return std::nullopt;
  const auto *object = std::get_if<core::JsonObjectDTO>(&found->second.value);
  const auto *policy = object == nullptr || !object->contains("policy")
                           ? nullptr
                           : std::get_if<std::string>(&object->at("policy").value);
  if (policy == nullptr || (*policy != "required" && *policy != "not_required") ||
      object->size() != 1U + static_cast<std::size_t>(object->contains("stall_window")))
    fail_retained_bundle("grasp_confirmation_invalid: policy must be required or not_required");
  GraspConfirmation result{*policy == "required", std::nullopt};
  if (const auto window = object->find("stall_window"); window != object->end()) {
    const auto *bounds = std::get_if<core::JsonObjectDTO>(&window->second.value);
    const auto number = [&](const std::string_view name) -> const double * {
      const auto value = bounds->find(name);
      return value == bounds->end() ? nullptr : std::get_if<double>(&value->second.value);
    };
    const auto *low = bounds == nullptr ? nullptr : number("min_position");
    const auto *high = bounds == nullptr ? nullptr : number("max_position");
    if (actuation != "position_commanded" || low == nullptr || bounds->size() != 2U ||
        high == nullptr || !std::isfinite(*low) || !std::isfinite(*high) || *low > *high)
      fail_retained_bundle(
          "grasp_confirmation_invalid: stall_window is a finite min_position <= max_position "
          "of a position gripper");
    result.stall_window = {*low, *high};
  }
  if (result.required && actuation == "position_commanded" && !result.stall_window)
    fail_retained_bundle(
        "grasp_confirmation_source_missing: a required position gripper needs stall_window");
  if (result.required && actuation == "pneumatic" &&
      (!pneumatic_io || !pneumatic_io->inputs.contains("workpiece_grasped")))
    fail_retained_bundle(
        "grasp_confirmation_source_missing: a required pneumatic gripper needs the "
        "pneumatic_io workpiece_grasped input");
  return result;
}

// Optional override of the component's Stop finish tolerance: a finite
// position-gripper distance >= 0 in the gripper's position units.
[[nodiscard]] std::optional<double> decode_stop_finish_tolerance(
    const core::JsonObjectDTO &fields, std::string_view actuation) {
  const auto found = fields.find("stop_finish_tolerance");
  if (found == fields.end()) return std::nullopt;
  const auto *value = std::get_if<double>(&found->second.value);
  if (actuation != "position_commanded" || value == nullptr || !std::isfinite(*value) ||
      *value < 0.0)
    fail_retained_bundle(
        "ros2_control runtime has an invalid gripper stop_finish_tolerance");
  return *value;
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
    auto pneumatic_io = decode_pneumatic_io(*fields, *kind, *mode);
    auto grasp_confirmation = decode_grasp_confirmation(*fields, *kind, pneumatic_io);
    const auto stop_finish_tolerance = decode_stop_finish_tolerance(*fields, *kind);
    auto common = *fields;
    common.erase("output_mode");
    common.erase("delivery_result_timeout_s");
    common.erase("pneumatic_io");
    common.erase("grasp_confirmation");
    common.erase("stop_finish_tolerance");
    auto decoded = decode_execution_gripper_target_configurations(
        core::JsonValueDTO::Array{core::JsonValueDTO{std::move(common)}},
        "ros2_control runtime configuration");
    result.push_back(Typed{std::move(decoded.front()), *mode == "none"
                            ? core::GripperOutputMode::kNone
                            : core::GripperOutputMode::kCommanded, delivery_timeout,
                            std::move(pneumatic_io), std::move(grasp_confirmation),
                            stop_finish_tolerance});
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

std::vector<ros2_control_execution_runtime_contract::Configuration::ComponentStream>
decode_ros2_control_execution_component_streams(
    const core::JsonObjectDTO &configuration) {
  namespace contract = ros2_control_execution_runtime_contract;
  using Key = std::pair<std::string, std::string>;
  static const core::JsonValueDTO::Array kNoRows;
  const auto rows = [&](const std::string_view name) -> const core::JsonValueDTO::Array & {
    const auto found = configuration.find(name);
    const auto *array = found == configuration.end()
                            ? nullptr
                            : std::get_if<core::JsonValueDTO::Array>(&found->second.value);
    return array == nullptr ? kNoRows : *array;
  };
  const auto member = [](const core::JsonValueDTO &row,
                         const std::string_view name) -> const core::JsonValueDTO * {
    const auto *object = std::get_if<core::JsonObjectDTO>(&row.value);
    const auto found = object == nullptr ? core::JsonObjectDTO::const_iterator{}
                                         : object->find(name);
    return object == nullptr || found == object->end() ? nullptr : &found->second;
  };
  const auto text = [&](const core::JsonValueDTO &row, const std::string_view name) {
    const auto *value = member(row, name);
    const auto *result = value == nullptr ? nullptr : std::get_if<std::string>(&value->value);
    if (result == nullptr) {
      fail_retained_bundle("ros2_control runtime has an invalid component stream row");
    }
    return *result;
  };
  // Every component with a controller profile declares its measured joints.
  // A pneumatic gripper without its own controller (Real robot, lane-free
  // tool without a profile) has no gripper-feedback stream; other grippers
  // keep theirs.
  std::set<Key> required;
  std::set<std::string> controlled_components;
  std::set<std::string> real_components;
  for (const auto &row : rows("controller_profiles")) {
    const auto component = text(row, "controllable_component_id");
    required.emplace(component,
                     std::string(contract::kMeasuredJointsStreamId));
    controlled_components.insert(component);
    if (text(row, "lane") == "real")
      real_components.insert(component);
  }
  for (const auto &row : rows("gripper_target_configurations")) {
    const auto component = text(row, "controllable_component_id");
    const bool own_controller = controlled_components.contains(component) &&
                                !real_components.contains(component);
    if (own_controller || text(row, "actuation_kind") != "pneumatic")
      required.emplace(component, std::string(contract::kGripperFeedbackStreamId));
  }
  std::vector<contract::Configuration::ComponentStream> result;
  std::set<Key> declared;
  for (const auto &row : rows("component_streams")) {
    const auto *object = std::get_if<core::JsonObjectDTO>(&row.value);
    const auto *budget_value = member(row, "budget_ns");
    const auto *budget =
        budget_value == nullptr ? nullptr : std::get_if<double>(&budget_value->value);
    if (object == nullptr || object->size() != 3U || budget == nullptr ||
        std::trunc(*budget) != *budget || *budget < 1e7 || *budget > 6e10) {
      fail_retained_bundle("ros2_control runtime has an invalid component stream row");
    }
    Key key{text(row, "controllable_component_id"), text(row, "stream_id")};
    if (!declared.insert(key).second) {
      fail_retained_bundle("ros2_control runtime repeats a component stream");
    }
    result.push_back({core::ControllableComponentId(key.first), key.second,
                      static_cast<std::int64_t>(*budget)});
  }
  if (declared != required) {
    fail_retained_bundle(
        "ros2_control runtime must declare exactly the measured_joints stream of "
        "every hosted component and the gripper_feedback stream of every "
        "gripper component except a pneumatic gripper without its own "
        "controller");
  }
  std::sort(result.begin(), result.end(), [](const auto &left, const auto &right) {
    return std::tie(left.controllable_component_id, left.stream_id) <
           std::tie(right.controllable_component_id, right.stream_id);
  });
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
    return std::nullopt; // Other providers retain their floor.
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
  std::vector<PneumaticIo> pneumatic_configs;
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
      auto io = decode_pneumatic_io(*fields, *kind, *mode);
      (void)decode_grasp_confirmation(*fields, *kind, io);
      if (io)
        pneumatic_configs.push_back(std::move(*io));
    }
  }
  (void)decode_ros2_control_execution_component_streams(unit.configuration);
  const auto io_catalog =
      ResolveRetainedIoSignalCatalog(graph, unit.runtime_unit_id);
  if (!io_catalog.ok()) {
    fail_retained_bundle(
        "ros2_control runtime has invalid retained I/O signals: " +
        io_catalog.error().details.at("reason"));
  }
  if (!pneumatic_configs.empty()) {
    // A gripper may reference signals on any I/O component in the retained
    // cell, including one hosted by a different runtime unit.
    const auto cell_catalog = ResolveRetainedIoSignalCatalog(graph);
    if (!cell_catalog.ok())
      fail_retained_bundle("pneumatic_io cannot resolve the retained I/O catalog");
    const auto require_signal = [&](const core::IoSignalId &id,
                                    const core::IoSignalDirection direction) {
      const auto &signals = cell_catalog.value();
      const auto match = std::find_if(signals.begin(), signals.end(), [&](const auto &row) {
        return row.descriptor.signal_id == id;
      });
      if (match == signals.end() ||
          std::count_if(signals.begin(), signals.end(), [&](const auto &row) {
            return row.descriptor.signal_id == id;
          }) != 1 || match->descriptor.direction != direction ||
          match->descriptor.value_type != core::IoSignalValueType::kBoolean) {
        fail_retained_bundle("pneumatic_io references a missing or incompatible signal: " +
                             id.controllable_component_id.value() + "/" + id.name);
      }
    };
    for (const auto &configuration : pneumatic_configs) {
      for (const auto &output : configuration.outputs)
        require_signal(output.signal, core::IoSignalDirection::kOutput);
      for (const auto &[unused, input] : configuration.inputs) {
        static_cast<void>(unused);
        require_signal(input, core::IoSignalDirection::kInput);
      }
    }
  }
  const auto io_devices =
      ResolveRetainedIoProviderDevices(graph, unit.runtime_unit_id);
  if (!io_devices.ok()) {
    fail_retained_bundle(
        "ros2_control runtime has invalid retained I/O devices: " +
        io_devices.error().details.at("reason"));
  }
  const auto pose_sources =
      ResolveRetainedPoseSources(graph, unit.runtime_unit_id);
  if (!pose_sources.ok()) {
    fail_retained_bundle(
        "ros2_control runtime has invalid retained pose sources: " +
        pose_sources.error().details.at("reason"));
  }
  const auto ai_models = ResolveRetainedAiModels(graph, unit.runtime_unit_id);
  if (!ai_models.ok()) {
    fail_retained_bundle(
        "ros2_control runtime has invalid retained AI models: " +
        ai_models.error().details.at("reason"));
  }
  const bool has_actuator = std::any_of(
      graph.control_bindings.begin(), graph.control_bindings.end(),
      [&](const auto &binding) {
        return binding.provider_runtime_unit_id == unit.runtime_unit_id &&
               (binding.capability_id.value() ==
                    "execution.trajectory.execute" ||
                binding.capability_id.value() == "tool.gripper.command");
      });
  // Sensors (`sensors` rows, checked by the closed schema): model-free too.
  std::set<std::string, std::less<>> sensor_components;
  if (const auto found = unit.configuration.find("sensors");
      found != unit.configuration.end())
    if (const auto *rows = std::get_if<core::JsonValueDTO::Array>(&found->second.value))
      for (const auto &row : *rows)
        if (const auto *object = std::get_if<core::JsonObjectDTO>(&row.value))
          if (const auto field = object->find("controllable_component_id");
              field != object->end())
            if (const auto *id = std::get_if<std::string>(&field->second.value))
              sensor_components.insert(*id);
  const bool io_only_host =
      !has_actuator &&
      std::all_of(
          graph.controllable_components.begin(),
          graph.controllable_components.end(), [&](const auto &component) {
            const bool hosted = std::find(component.runtime_unit_ids.begin(),
                                          component.runtime_unit_ids.end(),
                                          unit.runtime_unit_id) !=
                                component.runtime_unit_ids.end();
            // Model-free components: I/O devices, pose sources, AI models,
            // sensors.
            return !hosted ||
                   sensor_components.contains(
                       component.controllable_component_id.value()) ||
                   std::any_of(
                       io_catalog.value().begin(), io_catalog.value().end(),
                       [&](const auto &signal) {
                         return signal.runtime_unit_id ==
                                    unit.runtime_unit_id &&
                                signal.descriptor.signal_id.controllable_component_id ==
                                    component.controllable_component_id;
                       }) ||
                   std::any_of(
                       pose_sources.value().begin(), pose_sources.value().end(),
                       [&](const auto &source) {
                         return source.controllable_component_id ==
                                component.controllable_component_id;
                       }) ||
                   std::any_of(
                       ai_models.value().begin(), ai_models.value().end(),
                       [&](const auto &model) {
                         return model.controllable_component_id ==
                                component.controllable_component_id;
                       });
          });
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
      if (!has_motion && (field != "controller_profiles" || io_only_host)) {
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
      "controller-observation-only;per-component-state-authority;"
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
      "adapter-private;execution-host-status-v2;direct-child-epoch;"
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

/// The same-TU bridge for the contract decoder below.
[[nodiscard]] static CanonicalValue
canonical_from_json(const core::JsonValueDTO &value) {
  return json_value_to_canonical(value);
}

std::optional<core::StopLeadInAnchor>
decode_ros2_control_stop_lead_in_anchor(const std::optional<std::string> &value) {
  if (!value.has_value()) return std::nullopt;
  if (const auto anchor = core::stop_lead_in_anchor_from_string(*value)) return anchor;
  throw RetainedBundleError(
      "ros2_control controller profile has an unsupported stop_lead_in_anchor");
}

bool decode_ros2_control_tcp_offset_sync(const core::JsonValueDTO *value) {
  if (value == nullptr) return core::kDefaultControllerTcpOffsetSync;
  if (const auto *flag = std::get_if<bool>(&value->value)) return *flag;
  throw RetainedBundleError(
      "ros2_control controller profile controller_tcp_offset_sync must be a boolean");
}

} // namespace manyforge::deployment::detail

namespace manyforge::deployment::ros2_control_execution_runtime_contract {
namespace {

// The resolver already proved the ids, uniqueness and owner; the closed
// schema proved every field. Bindings keep the observer's order.
[[nodiscard]] Configuration::PoseSource
decode_pose_source(const RetainedPoseSourceConfiguration &source) {
  const auto row = detail::canonical_from_json(source.row);
  CanonicalValue::Object observer;
  for (const auto &[name, value] : row.as_object())
    if (name != "controllable_component_id")
      observer.emplace_back(name, value);
  Configuration::PoseSource result{source.controllable_component_id,
                                   CanonicalValue(std::move(observer)),
                                   {},
                                   std::nullopt};
  const auto text = [](const CanonicalValue &value, std::string_view name) {
    return value.find_object_member(name)->as_string();
  };
  const auto binding = [&](const CanonicalValue &value) {
    result.bindings.push_back(
        {text(value, "object_id"), text(value, "policy_id"),
         value.find_object_member("eligible")->as_bool(),
         static_cast<std::int64_t>(
             value.find_object_member("lifetime_ns")->as_number())});
  };
  if (const auto *joint = row.find_object_member("joint_state")) {
    binding(*joint);
    result.joint_state = Configuration::PoseSource::JointState{
        text(*joint, "object_id"), text(*joint, "policy_id"),
        text(*joint, "topic")};
  }
  for (const auto name : {"simulation_entities", "detection3d_sources"})
    for (const auto &value : row.find_object_member(name)->as_array())
      binding(value);
  return result;
}

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
  } else if (mode == "spring_back_fast") {
    profile.stop_mode = core::MotionInterruptStopMode::kSpringBackFast;
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

CanonicalValue empty_host_configuration() {
  CanonicalValue::Object configuration{
      {"planner_rng_seed", CanonicalValue(static_cast<double>(core::kDefaultPlannerRngSeed))},
      {"cartesian_timing", CanonicalValue(std::string(core::kDefaultCartesianTimingName))},
      {"execution_tick_hz", CanonicalValue(static_cast<double>(core::kDefaultExecutionTickHz))},
      {"collision_monitor_period_ms", CanonicalValue(core::kDefaultCollisionMonitorPeriodMs)},
      {"initial_joint_positions_rad", CanonicalValue(CanonicalValue::Array{})},
      {"motion_target_limits", CanonicalValue(CanonicalValue::Array{})},
      {"controller_profiles", CanonicalValue(CanonicalValue::Array{})},
  };
  return CanonicalValue(std::move(configuration));
}

CanonicalValue empty_host_deployment(
    const core::CellId &cell_id, const CanonicalValue &runtime_profile_reference,
    std::string_view deployment_id, std::string_view name) {
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
              {"configuration", empty_host_configuration()},
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

  const auto *profiles = std::get_if<core::JsonValueDTO::Array>(
      &require_member("controller_profiles").value);
  const auto *positions = std::get_if<core::JsonValueDTO::Array>(
      &require_member("initial_joint_positions_rad").value);
  if (profiles == nullptr || positions == nullptr ||
      (empty_host && (!profiles->empty() || !positions->empty()))) {
    throw RetainedBundleError("ros2_control runtime configuration has an "
                              "unsupported mode, profile, or initial state");
  }

  const auto canonical_configuration =
      detail::json_object_to_canonical(unit.configuration);
  detail::require_no_exclusive_ros_controller_topic_collision(
      canonical_configuration.find_object_member("controller_profiles")
          ->as_array(),
      [&]() -> const CanonicalValue::Array & {
        static const CanonicalValue::Array none;
        const auto *limits =
            canonical_configuration.find_object_member("motion_target_limits");
        return limits != nullptr ? limits->as_array() : none;
      }());
  Configuration result;
  // Q68: one controller profile per hosted component, one lane each.
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
    // The lane and model family name the model's `lanes.<lane>` row.
    auto lane = text("lane");
    auto model_family_id = text("model_family_id");
    // The model's `lanes.<lane>` row as canonical JSON of one object.
    auto lane_row = text("lane_row");
    if (lane_row.has_value()) {
      bool object_row = false;
      try {
        object_row = content_identity::parse_canonical_json(*lane_row).is_object();
      } catch (const content_identity::CanonicalValueError &) {
      }
      if (!object_row)
        throw RetainedBundleError(
            "ros2_control controller profile has an invalid lane_row");
    }
    // CP8 step 1: a Real row's site-settings address reference.
    auto device_address_ref = text("device_address_ref");
    const auto anchor_text = text("stop_lead_in_anchor");
    const auto stop_lead_in_anchor =
        detail::decode_ros2_control_stop_lead_in_anchor(anchor_text);
    // M6b: optional, a boolean when present (absent: on).
    const core::JsonValueDTO *tcp_offset_sync = nullptr;
    if (object != nullptr) {
      if (const auto found = object->find("controller_tcp_offset_sync"); found != object->end())
        tcp_offset_sync = &found->second;
    }
    const bool controller_tcp_offset_sync =
        detail::decode_ros2_control_tcp_offset_sync(tcp_offset_sync);
    const bool real = lane == std::optional<std::string>("real");
    auto owner_component_id = text("owner_component_id");
    auto member_role = text("member_role");
    // N17: the launched description, and an arm's joints or a tool's
    // gripper joint (exactly one).
    auto description = text("description");
    auto gripper_joint_name = text("gripper_joint_name");
    std::vector<std::string> joint_names;
    const core::JsonValueDTO::Array *joints = nullptr;
    if (object != nullptr)
      if (const auto found = object->find("joint_names"); found != object->end())
        joints = std::get_if<core::JsonValueDTO::Array>(&found->second.value);
    if (joints != nullptr)
      for (const auto &name : *joints) {
        const auto *joint = std::get_if<std::string>(&name.value);
        if (joint == nullptr || joint->empty())
          throw RetainedBundleError(
              "ros2_control controller profile has an invalid joint name");
        joint_names.push_back(*joint);
      }
    // Q48: an arm row's operator Stop decelerations (the closed schema proved
    // the rows: keyed by source joint, each positive).
    const auto field = [](const core::JsonObjectDTO *row,
                          std::string_view name) -> const core::JsonValueDTO * {
      if (row == nullptr) return nullptr;
      const auto found = row->find(name);
      return found == row->end() ? nullptr : &found->second;
    };
    std::map<std::string, double, std::less<>> max_deceleration_on_cancel;
    const auto *cancel = field(object, "max_deceleration_on_cancel_rad_s2");
    if (cancel != nullptr) {
      const auto *rows = std::get_if<core::JsonValueDTO::Array>(&cancel->value);
      if (rows == nullptr || rows->empty())
        throw RetainedBundleError("ros2_control controller profile has invalid "
                                  "max_deceleration_on_cancel_rad_s2");
      for (const auto &limit : *rows) {
        const auto *joint_row = std::get_if<core::JsonObjectDTO>(&limit.value);
        const auto *joint = field(joint_row, "source_joint_name");
        const auto *value = field(joint_row, "max_deceleration_rad_s2");
        const auto *name = joint == nullptr ? nullptr : std::get_if<std::string>(&joint->value);
        const auto *number = value == nullptr ? nullptr : std::get_if<double>(&value->value);
        if (name == nullptr || number == nullptr ||
            !max_deceleration_on_cancel.emplace(*name, *number).second)
          throw RetainedBundleError("ros2_control controller profile has invalid "
                                    "max_deceleration_on_cancel_rad_s2");
      }
    }
    if (!component.has_value() || !lane.has_value() ||
        !model_family_id.has_value() || !lane_row.has_value() ||
        !description.has_value() ||
        joint_names.empty() == !gripper_joint_name.has_value() ||
        joint_names.empty() == (cancel != nullptr) ||
        real != device_address_ref.has_value() ||
        owner_component_id.has_value() != member_role.has_value() ||
        object->size() != (object->contains("frame_prefix") ? 7U : 6U) +
                              (real ? 1U : 0U) +
                              (anchor_text.has_value() ? 1U : 0U) +
                              (tcp_offset_sync != nullptr ? 1U : 0U) +
                              (owner_component_id.has_value() ? 2U : 0U) +
                              (cancel != nullptr ? 1U : 0U)) {
      throw RetainedBundleError("ros2_control runtime configuration has an "
                                "unsupported mode, profile, or initial state");
    }
    Configuration::ControllerProfile profile{
        core::ControllableComponentId(*component), std::move(*lane),
        std::move(*lane_row), std::move(*model_family_id),
        std::move(frame_prefix)};
    profile.device_address_ref = std::move(device_address_ref);
    profile.stop_lead_in_anchor = stop_lead_in_anchor;
    profile.controller_tcp_offset_sync = controller_tcp_offset_sync;
    profile.owner_component_id = std::move(owner_component_id);
    profile.member_role = std::move(member_role);
    profile.joint_names = std::move(joint_names);
    profile.gripper_joint_name = std::move(gripper_joint_name);
    profile.description = std::move(*description);
    profile.max_deceleration_on_cancel_rad_s2 = std::move(max_deceleration_on_cancel);
    result.controller_profiles.push_back(std::move(profile));
  }
  std::sort(result.controller_profiles.begin(), result.controller_profiles.end(),
            [](const auto &left, const auto &right) {
              return left.controllable_component_id <
                     right.controllable_component_id;
            });
  // CONTRACTS §16: the machine owners (the closed schema proved each row).
  if (const auto found = unit.configuration.find("machines");
      found != unit.configuration.end()) {
    const auto *rows = std::get_if<core::JsonValueDTO::Array>(&found->second.value);
    if (rows == nullptr)
      throw RetainedBundleError("ros2_control runtime has invalid machines");
    for (const auto &row : *rows) {
      const auto *object = std::get_if<core::JsonObjectDTO>(&row.value);
      const auto text = [&](const std::string_view name) -> std::optional<std::string> {
        if (object == nullptr) return std::nullopt;
        const auto field = object->find(name);
        if (field == object->end()) return std::nullopt;
        const auto *value = std::get_if<std::string>(&field->second.value);
        if (value == nullptr || value->empty())
          throw RetainedBundleError("ros2_control runtime has an invalid machine row");
        return *value;
      };
      auto component = text("controllable_component_id");
      auto lane = text("lane");
      if (!component || !lane)
        throw RetainedBundleError("ros2_control runtime has an invalid machine row");
      result.machines.push_back({core::ControllableComponentId(*component),
                                 std::move(*lane), text("machine_lane_row"),
                                 text("device_address_ref")});
      // A deactivated arm on a lane where the machine has a process.
      auto arm_lane_row = text("arm_lane_row");
      auto arm_family = text("arm_model_family_id");
      auto arm_description = text("arm_description");
      const auto joints = object->find("arm_joint_names");
      if (arm_lane_row.has_value() != arm_description.has_value() ||
          arm_lane_row.has_value() != arm_family.has_value() ||
          arm_lane_row.has_value() != (joints != object->end()) ||
          (arm_lane_row && !result.machines.back().machine_lane_row))
        throw RetainedBundleError("ros2_control runtime has an invalid machine row");
      if (arm_lane_row) {
        Configuration::Machine::DeactivatedArm arm{std::move(*arm_lane_row),
                                                   std::move(*arm_family),
                                                   std::move(*arm_description), {}};
        const auto *names = std::get_if<core::JsonValueDTO::Array>(&joints->second.value);
        if (names == nullptr || names->empty())
          throw RetainedBundleError("ros2_control runtime has an invalid machine row");
        for (const auto &name : *names) {
          const auto *joint = std::get_if<std::string>(&name.value);
          if (joint == nullptr || joint->empty())
            throw RetainedBundleError("ros2_control runtime has an invalid machine row");
          arm.joint_names.push_back(*joint);
        }
        result.machines.back().deactivated_arm = std::move(arm);
      }
    }
  }
  // Sensors (the closed schema proved each row): one per component.
  if (const auto found = unit.configuration.find("sensors");
      found != unit.configuration.end()) {
    const auto *rows = std::get_if<core::JsonValueDTO::Array>(&found->second.value);
    if (rows == nullptr)
      throw RetainedBundleError("ros2_control runtime has invalid sensors");
    for (const auto &row : *rows) {
      const auto *object = std::get_if<core::JsonObjectDTO>(&row.value);
      const auto text = [&](const std::string_view name) -> std::optional<std::string> {
        if (object == nullptr) return std::nullopt;
        const auto field = object->find(name);
        if (field == object->end()) return std::nullopt;
        const auto *value = std::get_if<std::string>(&field->second.value);
        if (value == nullptr || value->empty())
          throw RetainedBundleError("ros2_control runtime has an invalid sensor row");
        return *value;
      };
      auto component = text("controllable_component_id");
      auto adapter_row = text("adapter_row");
      auto settings = text("settings");
      if (!component || !adapter_row || !settings)
        throw RetainedBundleError("ros2_control runtime has an invalid sensor row");
      result.sensors.push_back({core::ControllableComponentId(*component),
                                std::move(*adapter_row), std::move(*settings)});
    }
    std::sort(result.sensors.begin(), result.sensors.end(),
              [](const auto &left, const auto &right) {
                return left.controllable_component_id <
                       right.controllable_component_id;
              });
    if (std::adjacent_find(result.sensors.begin(), result.sensors.end(),
                           [](const auto &left, const auto &right) {
                             return left.controllable_component_id ==
                                    right.controllable_component_id;
                           }) != result.sensors.end())
      throw RetainedBundleError("ros2_control runtime repeats a sensor row");
  }
  const auto *motion_limits = std::get_if<core::JsonValueDTO::Array>(
      &require_member("motion_target_limits").value);
  if (motion_limits == nullptr ||
      (empty_host && !motion_limits->empty())) {
    throw RetainedBundleError(
        "ros2_control runtime requires motion target limits");
  }
  result.motion_target_limits =
      detail::decode_execution_motion_target_limits(*motion_limits);
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
    // Q48: exactly an arm's row carries its operator Stop decelerations.
    const bool arm = std::any_of(expected.begin(), expected.end(), [&](const auto &target) {
      return target.first == profile.controllable_component_id;
    });
    if (arm == profile.max_deceleration_on_cancel_rad_s2.empty()) {
      throw RetainedBundleError(
          "ros2_control controller profile must carry max_deceleration_on_cancel_rad_s2 "
          "exactly on an arm");
    }
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
  // D4: a lane-free pneumatic tool selects no controller profile. It hosts
  // no motion target, and each of its gripper targets is pneumatic with its
  // lane in referenced I/O (`pneumatic_io`) or no output at all.
  for (const auto &component : hosted_components) {
    if (profiled_components.contains(component)) continue;
    const bool lane_free =
        std::none_of(expected.begin(), expected.end(), [&](const auto &target) {
          return target.first == component;
        }) &&
        std::all_of(expected_grippers.begin(), expected_grippers.end(), [&](const auto &target) {
          return target.first != component ||
                 std::any_of(result.gripper_target_configurations.begin(),
                             result.gripper_target_configurations.end(), [&](const auto &row) {
                               return row.controllable_component_id == component &&
                                      row.command_target_id == target.second &&
                                      row.actuation_kind == "pneumatic" &&
                                      (row.pneumatic_io.has_value() ||
                                       row.output_mode == core::GripperOutputMode::kNone);
                             });
        });
    if (lane_free) profiled_components.insert(component);
  }
  if (profiled_components != hosted_components) {
    throw RetainedBundleError(
        "ros2_control runtime must select exactly one controller profile for "
        "every hosted component and no unknown component");
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
  const auto pose_sources = ResolveRetainedPoseSources(
      retained_bundle.value().cell_graph, runtime_unit_id);
  if (!pose_sources.ok()) {
    throw RetainedBundleError(
        "ros2_control runtime has invalid pose sources: " +
        pose_sources.error().details.at("reason"));
  }
  for (const auto &source : pose_sources.value())
    result.pose_sources.push_back(decode_pose_source(source));
  const auto ai_models = ResolveRetainedAiModels(
      retained_bundle.value().cell_graph, runtime_unit_id);
  if (!ai_models.ok()) {
    throw RetainedBundleError(
        "ros2_control runtime has invalid AI models: " +
        ai_models.error().details.at("reason"));
  }
  for (const auto &model : ai_models.value())
    result.ai_models.push_back({model.controllable_component_id,
                                model.base_url, model.model,
                                model.api_key_env});
  result.component_streams =
      detail::decode_ros2_control_execution_component_streams(unit.configuration);
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
  result.collision_clearance_m = optional_number("collision_clearance_m");
  result.collision_check_step_rad = optional_number("collision_check_step_rad");
  if (const auto found = unit.configuration.find("collision_check_fresh_plans");
      found != unit.configuration.end()) {
    const auto *flag = std::get_if<bool>(&found->second.value);
    if (flag == nullptr) {
      throw RetainedBundleError(
          "ros2_control runtime configuration has invalid "
          "collision_check_fresh_plans");
    }
    result.collision_check_fresh_plans = *flag;
  }
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
  std::erase_if(result.machines, unrelated);
  std::erase_if(result.sensors, unrelated);
  std::erase_if(result.pose_sources, unrelated);
  std::erase_if(result.ai_models, unrelated);
  std::erase_if(result.default_fault_stop_profiles, unrelated);
  std::erase_if(result.component_streams, unrelated);
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
    for (auto &[name, value] : result) {
      if (name == "initial_joint_positions_rad") {
        CanonicalValue::Array positions;
        for (const auto position : selected.initial_joint_positions_rad) positions.emplace_back(position);
        value = CanonicalValue(std::move(positions));
      } else if (name == "controller_profiles" ||
                 name == "motion_target_limits" ||
                 name == "default_fault_stop_profiles" ||
                 name == "gripper_target_configurations" ||
                 name == "io_signals" || name == "io_provider_devices" ||
                 name == "pose_sources" || name == "ai_models" ||
                 name == "component_streams" ||
                 name == "lifecycle_command_timeouts" || name == "machines" ||
                 name == "sensors") {
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
    // Another component's lifecycle budget leaves this one's identity as it
    // was without any budget (no replacement when only a budget changes).
    std::erase_if(result, [](const auto &field) {
      return field.first == "lifecycle_command_timeouts" &&
             field.second.as_array().empty();
    });
    return CanonicalValue(std::move(result));
  }
  throw RetainedBundleError("ROS component configuration has no runtime projection");
}

std::map<std::string, std::int64_t, std::less<>>
component_lifecycle_budgets_ns(const core::CellGraphDTO &graph) {
  std::map<std::string, std::int64_t, std::less<>> result;
  // The closed runtime schema bounds every value to (0, 3600] s.
  for (const auto &unit : graph.runtime_units) {
    for (const std::string_view field :
         {"lifecycle_command_timeouts", "io_provider_devices"}) {
      const auto found = unit.configuration.find(field);
      const auto *rows =
          found == unit.configuration.end()
              ? nullptr
              : std::get_if<core::JsonValueDTO::Array>(&found->second.value);
      if (rows == nullptr) continue;
      for (const auto &item : *rows) {
        const auto *row = std::get_if<core::JsonObjectDTO>(&item.value);
        if (row == nullptr) continue;
        const auto component = row->find("controllable_component_id");
        const auto seconds = row->find("lifecycle_command_timeout_s");
        if (component == row->end() || seconds == row->end()) continue;
        const auto *id = std::get_if<std::string>(&component->second.value);
        const auto *value = std::get_if<double>(&seconds->second.value);
        if (id == nullptr || value == nullptr) continue;
        result.insert_or_assign(
            *id, static_cast<std::int64_t>(std::ceil(
                     static_cast<long double>(*value) * 1'000'000'000.0L)));
      }
    }
  }
  return result;
}

std::map<std::string, std::int64_t, std::less<>>
component_composition_budgets_ns(const core::CellGraphDTO &graph) {
  auto result = component_lifecycle_budgets_ns(graph);
  for (const auto &component : graph.controllable_components)
    result.try_emplace(component.controllable_component_id.value(),
                       kDefaultLifecycleBudgetNs);
  return result;
}

std::string
pose_source_stream_id(const Configuration::PoseSource::Binding &binding) {
  return PoseSourceStreamId(binding.object_id, binding.policy_id);
}

const Configuration::PoseSource *
find_pose_source(const Configuration &configuration,
                 const core::ControllableComponentId &component_id) {
  const auto found = std::find_if(
      configuration.pose_sources.begin(), configuration.pose_sources.end(),
      [&](const auto &source) {
        return source.controllable_component_id == component_id;
      });
  return found == configuration.pose_sources.end() ? nullptr : &*found;
}

const Configuration::AiModel *
find_ai_model(const Configuration &configuration,
              const core::ControllableComponentId &component_id) {
  const auto found = std::find_if(
      configuration.ai_models.begin(), configuration.ai_models.end(),
      [&](const auto &model) {
        return model.controllable_component_id == component_id;
      });
  return found == configuration.ai_models.end() ? nullptr : &*found;
}

const Configuration::Sensor *
find_sensor(const Configuration &configuration,
            const core::ControllableComponentId &component_id) {
  const auto found = std::find_if(
      configuration.sensors.begin(), configuration.sensors.end(),
      [&](const auto &sensor) {
        return sensor.controllable_component_id == component_id;
      });
  return found == configuration.sensors.end() ? nullptr : &*found;
}

const Configuration::ComponentStream *
find_component_stream(const Configuration &configuration,
                      const core::ControllableComponentId &component,
                      const std::string_view stream_id) {
  const auto found = std::find_if(
      configuration.component_streams.begin(),
      configuration.component_streams.end(), [&](const auto &stream) {
        return stream.controllable_component_id == component &&
               stream.stream_id == stream_id;
      });
  return found == configuration.component_streams.end() ? nullptr : &*found;
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

ControllerSimulatorDependency
controller_simulator_dependency(const Configuration::ControllerProfile &profile) {
  return profile.lane == "isaac" ? ControllerSimulatorDependency::kIsaac
                                 : ControllerSimulatorDependency::kNone;
}

std::string_view to_string(const ControllerSimulatorDependency dependency) noexcept {
  return dependency == ControllerSimulatorDependency::kIsaac ? "isaac" : "none";
}

CanonicalValue component_simulator_dependencies(const RetainedBundle &bundle) {
  std::vector<std::pair<std::string, std::string_view>> rows;
  for (const auto &unit : bundle.value().cell_graph.runtime_units) {
    if (unit.runtime_profile_id.value() != kRuntimeProfileId) continue;
    for (const auto &profile :
         require_configuration(bundle, unit.runtime_unit_id).controller_profiles) {
      rows.emplace_back(profile.controllable_component_id.value(),
                        to_string(controller_simulator_dependency(profile)));
    }
  }
  std::sort(rows.begin(), rows.end());
  if (std::adjacent_find(rows.begin(), rows.end(), [](const auto &left, const auto &right) {
        return left.first == right.first;
      }) != rows.end()) {
    throw RetainedBundleError("ROS controller profiles repeat a component");
  }
  CanonicalValue::Array result;
  result.reserve(rows.size());
  for (const auto &[component, simulator] : rows) {
    result.emplace_back(CanonicalValue::Object{
        {"controllable_component_id", CanonicalValue(component)},
        {"simulator", CanonicalValue(std::string(simulator))},
    });
  }
  return CanonicalValue(std::move(result));
}

std::string implementation_revision() {
  return detail::ros2_control_execution_runtime_implementation_revision();
}

} // namespace manyforge::deployment::ros2_control_execution_runtime_contract
