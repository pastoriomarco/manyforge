#include "native_execution_runtime_unit_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <type_traits>
#include <limits>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/closed_schema.hpp>
#include <manyforge_deployment/content_identity.hpp>

#include "verified_retained_bundle_internal.hpp"

namespace manyforge::deployment::detail {
namespace {

constexpr std::string_view kMotionExecutionCapabilityId =
    "execution.trajectory.execute";
constexpr std::string_view kGripperCommandCapabilityId = "tool.gripper.command";

constexpr std::string_view kRuntimeProfileConfigurationSchemaId =
    kNativeExecutionRuntimeConfigurationSchemaId;
constexpr std::string_view kRuntimeProfileConfigurationSchemaRevision =
    kNativeExecutionRuntimeConfigurationSchemaRevision;

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
        } else if constexpr (std::is_same_v<Value, core::JsonValueDTO::Array>) {
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
        } else if constexpr (std::is_same_v<Value, CanonicalValue::Array>) {
          core::JsonValueDTO::Array array;
          array.reserve(current.size());
          for (const auto &item : current) {
            array.push_back(canonical_to_json_value(item));
          }
          return core::JsonValueDTO{std::move(array)};
        } else {
          return core::JsonValueDTO{
              canonical_to_json_object(value, "runtime_configuration")};
        }
      },
      value.storage());
}

[[nodiscard]] CanonicalValue require_qualified_runtime_configuration(
    const core::JsonObjectDTO &configuration) {
  const auto &registry = build_qualified_closed_schema_registry();
  const auto *schema = registry.find_qualified_value_schema(
      core::SchemaId(std::string(kRuntimeProfileConfigurationSchemaId)),
      core::SchemaRevision(
          std::string(kRuntimeProfileConfigurationSchemaRevision)));
  if (schema == nullptr) {
    fail_retained_bundle(
        "native execution configuration schema is absent from this build");
  }
  const CanonicalValue candidate = json_object_to_canonical(configuration);
  try {
    const auto normalized =
        validate_and_normalize_build_qualified_value(*schema, candidate);
    if (normalized.normalized_value() != candidate) {
      fail_retained_bundle(
          "native execution configuration is not in canonical normalized form");
    }
    return normalized.normalized_value();
  } catch (const ClosedSchemaError &error) {
    fail_retained_bundle(std::string("native execution configuration: ") +
                         error.what());
  } catch (const CanonicalValueError &error) {
    fail_retained_bundle(std::string("native execution configuration: ") +
                         error.what());
  }
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
        "native execution product has no exact retained graph subject");
  }
  return *found;
}

} // namespace

// DX-753: archival Draft/import metadata only; live Native admission is withdrawn.
std::string native_execution_runtime_profile_document() {
  CanonicalValue identity(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.runtime_profile.v1")},
      {"runtime_profile_id",
       CanonicalValue(std::string(kNativeExecutionRuntimeProfileId))},
      {"configuration_schema_ref",
       CanonicalValue(CanonicalValue::Object{
           {"schema_id",
            CanonicalValue(std::string(kRuntimeProfileConfigurationSchemaId))},
           {"schema_revision",
            CanonicalValue(
                std::string(kRuntimeProfileConfigurationSchemaRevision))},
       })},
      {"process_contract",
       CanonicalValue(CanonicalValue::Object{
           {"lifecycle_control", CanonicalValue("manager_managed")},
       })},
      {"supervision_contract",
       CanonicalValue(CanonicalValue::Object{
           {"status_endpoint_role_id",
            CanonicalValue(
                std::string(kNativeExecutionRuntimeStatusEndpointRoleId))},
       })},
      {"offered_runtime_role_ids",
       CanonicalValue(CanonicalValue::Array{
           CanonicalValue(std::string(kNativeExecutionRuntimeRoleId))})},
      {"source_endpoint_role_ids",
       CanonicalValue(CanonicalValue::Array{
           CanonicalValue(
               std::string(kNativeExecutionRuntimeLogicalSceneEndpointRoleId)),
           CanonicalValue(
               std::string(kNativeExecutionRuntimeStatusEndpointRoleId)),
           CanonicalValue(
               std::string(kNativeExecutionRuntimeProjectionEndpointRoleId)),
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

std::string native_execution_runtime_profile_revision() {
  // A pure function of build constants: derive it once per process.
  static const std::string revision = [] {
    const auto root = parse_strict_json_model_yaml(
                          native_execution_runtime_profile_document())
                          .root();
    return require_string(
        require_member(
            require_object(root, "native_execution_runtime_profile"),
            "runtime_profile_revision", "native_execution_runtime_profile"),
        "native_execution_runtime_profile.runtime_profile_revision");
  }();
  return revision;
}

// DX-753: held component-identity schema validation only.
bool native_execution_runtime_configuration_is_qualified(
    const core::JsonObjectDTO &configuration) noexcept {
  try {
    static_cast<void>(require_qualified_runtime_configuration(configuration));
    return true;
  } catch (...) {
    return false;
  }
}

std::vector<execution_runtime_contract::Configuration::MotionTargetLimit>
decode_execution_motion_target_limits(
    const core::JsonValueDTO::Array &motion_target_limits,
    const bool include_ordinary_jerk) {
  using execution_runtime_contract::Configuration;
  std::vector<Configuration::MotionTargetLimit> result;
  result.reserve(motion_target_limits.size());
  for (const auto &entry : motion_target_limits) {
    const auto *object = std::get_if<core::JsonObjectDTO>(&entry.value);
    // `tcp_speed_check` is schema-materialized; absent keeps its default.
    const bool has_speed_check =
        object != nullptr && object->find("tcp_speed_check") != object->end();
    if (object == nullptr ||
        object->size() != (include_ordinary_jerk ? 4U : 3U) +
                               (has_speed_check ? 1U : 0U)) {
      throw RetainedBundleError(
          "execution runtime configuration has an invalid motion target limit");
    }
    auto tcp_speed_check = core::TcpSpeedCheck::kActiveTcpAndFlange;
    if (has_speed_check) {
      const auto *value =
          std::get_if<std::string>(&object->at("tcp_speed_check").value);
      const auto parsed = value == nullptr
                              ? std::nullopt
                              : core::parse_tcp_speed_check(*value);
      if (!parsed) {
        throw RetainedBundleError(
            "execution runtime configuration has an invalid tcp_speed_check");
      }
      tcp_speed_check = *parsed;
    }
    const auto component = object->find("controllable_component_id");
    const auto target = object->find("command_target_id");
    const auto limit = object->find("maximum_tcp_linear_speed_mps");
    if (component == object->end() || target == object->end() ||
        limit == object->end()) {
      throw RetainedBundleError("execution runtime configuration has an "
                                "incomplete motion target limit");
    }
    const auto *component_id =
        std::get_if<std::string>(&component->second.value);
    const auto *target_id = std::get_if<std::string>(&target->second.value);
    const auto *maximum = std::get_if<double>(&limit->second.value);
    if (component_id == nullptr || component_id->empty() ||
        target_id == nullptr || target_id->empty() || maximum == nullptr ||
        !std::isfinite(*maximum) || *maximum <= 0.0 || *maximum > 1000.0) {
      throw RetainedBundleError(
          "execution runtime configuration has an invalid "
          "motion target limit value");
    }
    result.push_back(Configuration::MotionTargetLimit{
        core::ControllableComponentId(*component_id),
        core::CommandTargetId(*target_id), *maximum,
        include_ordinary_jerk
            ? detail::decode_ordinary_motion_jerk_settings(
                  std::get<core::JsonObjectDTO>(
                      object->at("ordinary_motion_jerk").value))
            : execution_runtime_contract::OrdinaryMotionJerkSettings{},
        tcp_speed_check});
  }
  std::sort(result.begin(), result.end(),
            [](const auto &left, const auto &right) {
              return std::tie(left.controllable_component_id,
                              left.command_target_id) <
                     std::tie(right.controllable_component_id,
                              right.command_target_id);
            });
  if (std::adjacent_find(result.begin(), result.end(),
                         [](const auto &left, const auto &right) {
                           return left.controllable_component_id ==
                                      right.controllable_component_id &&
                                  left.command_target_id ==
                                      right.command_target_id;
                         }) != result.end()) {
    throw RetainedBundleError(
        "execution runtime configuration repeats a motion target limit");
  }
  return result;
}

bool execution_gripper_command_duration_is_valid(
    const std::string_view actuation_kind, const double duration_s) noexcept {
  return std::isfinite(duration_s) && duration_s <= 60.0 &&
         ((actuation_kind == "pneumatic" && duration_s >= 0.0) ||
          (actuation_kind == "position_commanded" && duration_s > 0.0));
}

std::vector<
    execution_runtime_contract::Configuration::GripperTargetConfiguration>
decode_execution_gripper_target_configurations(
    const core::JsonValueDTO::Array &gripper_target_configurations,
    const std::string_view label) {
  using execution_runtime_contract::Configuration;
  std::vector<Configuration::GripperTargetConfiguration> result;
  result.reserve(
      gripper_target_configurations.size());
  for (const auto &entry : gripper_target_configurations) {
    const auto *object = std::get_if<core::JsonObjectDTO>(&entry.value);
    if (object == nullptr || object->size() != 12U) {
      throw RetainedBundleError(
          std::string(label) + " has an invalid gripper target configuration");
    }
    const auto string_member = [&](const std::string_view name) -> std::string {
      const auto found = object->find(name);
      if (found == object->end()) {
        throw RetainedBundleError(
            std::string(label) + " has an incomplete gripper target configuration");
      }
      const auto *value = std::get_if<std::string>(&found->second.value);
      if (value == nullptr || value->empty()) {
        throw RetainedBundleError(
            std::string(label) + " has an invalid gripper target string");
      }
      return *value;
    };
    const auto string_array_member =
        [&](const std::string_view name, const bool require_nonempty) {
          const auto found = object->find(name);
          const auto *values = found == object->end()
                                   ? nullptr
                                   : std::get_if<core::JsonValueDTO::Array>(
                                         &found->second.value);
          if (values == nullptr || (require_nonempty && values->empty())) {
            throw RetainedBundleError(
                std::string(label) + " has an invalid gripper target list");
          }
          std::vector<std::string> result_values;
          result_values.reserve(values->size());
          for (const auto &value : *values) {
            const auto *text = std::get_if<std::string>(&value.value);
            if (text == nullptr || text->empty()) {
              throw RetainedBundleError(
                  std::string(label) + " has an invalid gripper target list item");
            }
            result_values.push_back(*text);
          }
          if (!std::is_sorted(result_values.begin(), result_values.end()) ||
              std::adjacent_find(result_values.begin(), result_values.end()) !=
                  result_values.end()) {
            throw RetainedBundleError(
                std::string(label) + " has a noncanonical gripper target list");
          }
          return result_values;
        };
    const auto duration = object->find("command_duration_s");
    const auto *duration_s =
        duration == object->end()
            ? nullptr
            : std::get_if<double>(&duration->second.value);
    const auto component_id = string_member("controllable_component_id");
    const auto target_id = string_member("command_target_id");
    const auto actuation_kind = string_member("actuation_kind");
    if (duration_s == nullptr ||
        !execution_gripper_command_duration_is_valid(actuation_kind,
                                                     *duration_s)) {
      throw RetainedBundleError(
          std::string(label) + " has invalid gripper target semantics");
    }
    result.push_back(Configuration::GripperTargetConfiguration{
        core::ControllableComponentId(component_id), core::CommandTargetId(target_id),
        string_member("description"), actuation_kind, string_member("open_state_ref"),
        string_member("closed_state_ref"), *duration_s, string_array_member("touch_links", true),
        string_member("mount_frame"), string_member("attach_frame"), string_member("tcp_frame"),
        string_array_member("auxiliary_frames", false)});
  }
  std::sort(result.begin(),
            result.end(),
            [](const auto &left, const auto &right) {
              return std::tie(left.controllable_component_id,
                              left.command_target_id) <
                     std::tie(right.controllable_component_id,
                              right.command_target_id);
            });
  if (std::adjacent_find(
          result.begin(),
          result.end(),
          [](const auto &left, const auto &right) {
            return left.controllable_component_id ==
                       right.controllable_component_id &&
                   left.command_target_id == right.command_target_id;
          }) != result.end()) {
    throw RetainedBundleError(
        std::string(label) + " repeats a gripper target configuration");
  }
  return result;
}

// DX-753: retain the exact decoder for held per-component Apply scope.
execution_runtime_contract::Configuration
decode_native_execution_runtime_configuration(
    const core::JsonObjectDTO &configuration, const core::CellGraphDTO &graph,
    const core::RuntimeUnitId &runtime_unit_id) {
  using execution_runtime_contract::Configuration;
  const auto require_member =
      [&](const std::string_view name) -> const core::JsonValueDTO & {
    const auto found = configuration.find(name);
    if (found == configuration.end()) {
      throw RetainedBundleError("native runtime configuration is missing " +
                                std::string(name));
    }
    return found->second;
  };
  const auto require_number = [&](const std::string_view name) {
    const auto *number = std::get_if<double>(&require_member(name).value);
    if (number == nullptr || !std::isfinite(*number)) {
      throw RetainedBundleError("native runtime configuration has invalid " +
                                std::string(name));
    }
    return *number;
  };
  const auto require_uint32 = [&](const std::string_view name) {
    const double number = require_number(name);
    if (std::trunc(number) != number || number < 0.0 ||
        number >
            static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
      throw RetainedBundleError("native runtime configuration has invalid " +
                                std::string(name));
    }
    return static_cast<std::uint32_t>(number);
  };

  const auto *mode = std::get_if<std::string>(&require_member("mode").value);
  const auto *positions = std::get_if<core::JsonValueDTO::Array>(
      &require_member("initial_joint_positions_rad").value);
  const auto *motion_target_limits = std::get_if<core::JsonValueDTO::Array>(
      &require_member("motion_target_limits").value);
  const auto *direct_control_joint_limits =
      std::get_if<core::JsonValueDTO::Array>(
          &require_member("direct_control_joint_limits").value);
  const auto gripper_configurations_member =
      configuration.find("gripper_target_configurations");
  const auto *gripper_target_configurations =
      gripper_configurations_member == configuration.end()
          ? nullptr
          : std::get_if<core::JsonValueDTO::Array>(
                &gripper_configurations_member->second.value);
  // Motion-only fields are required exactly when this unit hosts a motion
  // capability (H1a-IF.2). A capability-only unit (for example gripper-only)
  // carries them empty; placeholder joints or TCP limits are refused.
  const auto hosts = [&](const core::ControllableComponentId &component,
                         const core::CommandTargetId &target,
                         const std::string_view capability) {
    return std::any_of(
        graph.control_bindings.begin(), graph.control_bindings.end(),
        [&](const auto &binding) {
          return binding.provider_runtime_unit_id == runtime_unit_id &&
                 binding.capability_id.value() == capability &&
                 (component.empty() ||
                  (binding.controllable_component_id == component &&
                   binding.command_target_id == target));
        });
  };
  const bool motion_advertised = std::any_of(
      graph.control_bindings.begin(), graph.control_bindings.end(),
      [&](const auto &binding) {
        return binding.provider_runtime_unit_id == runtime_unit_id &&
               binding.capability_id.value() == kMotionExecutionCapabilityId;
      });
  if (mode == nullptr || *mode != "simulated" || positions == nullptr ||
      motion_target_limits == nullptr || direct_control_joint_limits == nullptr ||
      positions->empty() != !motion_advertised ||
      motion_target_limits->empty() != !motion_advertised ||
      (!motion_advertised && !direct_control_joint_limits->empty())) {
    throw RetainedBundleError(
        "native runtime configuration has an unsupported mode or initial "
        "state for its advertised capabilities");
  }

  Configuration result;
  result.planner_rng_seed = require_uint32("planner_rng_seed");
  result.execution_tick_hz = require_uint32("execution_tick_hz");
  result.collision_monitor_period_ms =
      require_number("collision_monitor_period_ms");
  result.initial_joint_positions_rad.reserve(positions->size());
  for (const auto &position : *positions) {
    const auto *number = std::get_if<double>(&position.value);
    if (number == nullptr || !std::isfinite(*number)) {
      throw RetainedBundleError(
          "native runtime configuration has an invalid initial joint state");
    }
    result.initial_joint_positions_rad.push_back(*number);
  }
  result.motion_target_limits =
      decode_execution_motion_target_limits(*motion_target_limits, true);
  result.direct_control_joint_limits.reserve(
      direct_control_joint_limits->size());
  for (const auto &entry : *direct_control_joint_limits) {
    const auto *object = std::get_if<core::JsonObjectDTO>(&entry.value);
    if (object == nullptr || object->size() < 4U || object->size() > 6U) {
      throw RetainedBundleError(
          "native runtime configuration has an invalid direct-control joint limit");
    }
    const auto component = object->find("controllable_component_id");
    const auto target = object->find("command_target_id");
    const auto joint = object->find("source_joint_name");
    if (component == object->end() || target == object->end() ||
        joint == object->end()) {
      throw RetainedBundleError(
          "native runtime configuration has an incomplete direct-control joint limit");
    }
    const auto *component_id =
        std::get_if<std::string>(&component->second.value);
    const auto *target_id = std::get_if<std::string>(&target->second.value);
    const auto *source_joint_name =
        std::get_if<std::string>(&joint->second.value);
    if (component_id == nullptr || component_id->empty() ||
        target_id == nullptr || target_id->empty() ||
        source_joint_name == nullptr || source_joint_name->empty()) {
      throw RetainedBundleError(
          "native runtime configuration has an invalid direct-control joint identity");
    }
    const auto optional_positive = [&](const std::string_view name) {
      const auto found = object->find(name);
      if (found == object->end()) {
        return std::optional<double>{};
      }
      const auto *value = std::get_if<double>(&found->second.value);
      if (value == nullptr || !std::isfinite(*value) || *value <= 0.0) {
        throw RetainedBundleError(
            "native runtime configuration has an invalid direct-control joint limit value");
      }
      return std::optional<double>{*value};
    };
    const auto velocity = optional_positive("max_velocity_rad_s");
    const auto acceleration = optional_positive("max_acceleration_rad_s2");
    const auto jerk = optional_positive("max_jerk_rad_s3");
    if (!velocity.has_value() && !acceleration.has_value() &&
        !jerk.has_value()) {
      throw RetainedBundleError(
          "native runtime configuration has an empty direct-control joint limit");
    }
    result.direct_control_joint_limits.push_back(
        Configuration::DirectControlJointLimit{
            core::ControllableComponentId(*component_id),
            core::CommandTargetId(*target_id), *source_joint_name, velocity,
            acceleration, jerk});
  }
  std::sort(result.direct_control_joint_limits.begin(),
            result.direct_control_joint_limits.end(),
            [](const auto &left, const auto &right) {
              return std::tie(left.controllable_component_id,
                              left.command_target_id,
                              left.source_joint_name) <
                     std::tie(right.controllable_component_id,
                              right.command_target_id,
                              right.source_joint_name);
            });
  if (std::adjacent_find(
          result.direct_control_joint_limits.begin(),
          result.direct_control_joint_limits.end(),
          [](const auto &left, const auto &right) {
            return left.controllable_component_id ==
                       right.controllable_component_id &&
                   left.command_target_id == right.command_target_id &&
                   left.source_joint_name == right.source_joint_name;
          }) != result.direct_control_joint_limits.end()) {
    throw RetainedBundleError(
        "native runtime configuration repeats a direct-control joint limit");
  }
  // Direct-control limits apply only to exact motion-execution targets hosted
  // by this unit; they cannot qualify a gripper or foreign component target.
  for (const auto &limit : result.direct_control_joint_limits) {
    if (!hosts(limit.controllable_component_id, limit.command_target_id,
               kMotionExecutionCapabilityId)) {
      throw RetainedBundleError(
          "native runtime direct-control limit names a target this unit does "
          "not host for motion execution");
    }
  }
  // Each motion limit names a motion target this unit actually hosts.
  for (const auto &limit : result.motion_target_limits) {
    if (!hosts(limit.controllable_component_id, limit.command_target_id,
               kMotionExecutionCapabilityId)) {
      throw RetainedBundleError(
          "native runtime motion limit names a target this unit does not host");
    }
  }
  if (gripper_configurations_member != configuration.end() &&
      gripper_target_configurations == nullptr) {
    throw RetainedBundleError(
        "native runtime configuration has invalid gripper target configurations");
  }
  if (gripper_target_configurations != nullptr) {
    result.gripper_target_configurations =
        decode_execution_gripper_target_configurations(
            *gripper_target_configurations, "native runtime configuration");
  }
  // Each gripper configuration names a gripper target this unit hosts, on
  // its own component; it never borrows an arm's identity.
  for (const auto &gripper : result.gripper_target_configurations) {
    if (!hosts(gripper.controllable_component_id, gripper.command_target_id,
               kGripperCommandCapabilityId)) {
      throw RetainedBundleError(
          "native runtime gripper configuration names a target this unit does "
          "not host");
    }
  }
  if (result.execution_tick_hz == 0U ||
      !(result.collision_monitor_period_ms > 0.0)) {
    throw RetainedBundleError(
        "native runtime configuration has an invalid cadence");
  }
  return result;
}

} // namespace manyforge::deployment::detail

namespace manyforge::deployment::native_execution_runtime_contract {
// DX-753: retain exact held component-projection guards, not positive admission.
Configuration
require_configuration(const RetainedBundle &retained_bundle,
                      const core::RuntimeUnitId &runtime_unit_id) {
  if (!verify_retained_bundle_identity(retained_bundle)) {
    throw RetainedBundleError(
        "native runtime configuration received an unverifiable bundle");
  }
  const auto &unit = detail::require_retained_runtime_unit(
      retained_bundle.value(), runtime_unit_id);
  if (unit.runtime_profile_id.value() !=
          detail::kNativeExecutionRuntimeProfileId ||
      unit.runtime_profile_revision.value() !=
          detail::native_execution_runtime_profile_revision() ||
      !unit.dependency_runtime_unit_ids.empty() ||
      !detail::native_execution_runtime_configuration_is_qualified(
          unit.configuration)) {
    throw RetainedBundleError(
        "native runtime unit differs from the build-qualified profile");
  }

  return detail::decode_native_execution_runtime_configuration(
      unit.configuration, retained_bundle.value().cell_graph, runtime_unit_id);
}

} // namespace manyforge::deployment::native_execution_runtime_contract
