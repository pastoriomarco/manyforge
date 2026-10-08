#include "activation_deployment_compiler_internal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <map>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <manyforge_core/types/cell_graph.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/cell_graph_codec.hpp>
#include <manyforge_deployment/deployment_artifact_codec.hpp>
#include <manyforge_deployment/native_domain_topology.hpp>
#include <manyforge_deployment/native_execution_runtime_contract.hpp>
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>

namespace manyforge::deployment::detail {
namespace {

using Array = CanonicalValue::Array;
using Object = CanonicalValue::Object;

constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";

constexpr std::array<std::string_view, 19U> kNativeArmCapabilities{{
    "execution.contact.geometric",
    "execution.contact.geometric_signed_penetration",
    "execution.feedback.phase_events",
    "execution.feedback.state_snapshots",
    "execution.reset",
    "execution.runtime_scale.operation_scoped",
    "execution.stop.soft",
    "execution.trajectory.execute",
    "model.kinematics.fk",
    "model.limits.joint",
    "planning.motion.cartesian_path",
    "planning.motion.joint_goal",
    "planning.motion.pose_goal",
    "planning.profile.balanced",
    "planning.profile.first_feasible",
    "planning.profile.quality_bounded",
    "planning.trajectory.time_parameterized",
    "state.observed.current",
    "validation.trajectory.non_collision",
}};
constexpr std::array<std::string_view, 4U> kGripperCapabilities{{
    "model.kinematics.fk", "model.limits.joint", "tool.gripper.command",
    "tool.gripper.feedback",
}};
constexpr auto kRosArmCapabilities = [] {
  std::array<std::string_view, kNativeArmCapabilities.size() - 1U> result{};
  std::size_t index = 0U;
  for (const auto capability : kNativeArmCapabilities) {
    if (capability != "execution.runtime_scale.operation_scoped") {
      result[index++] = capability;
    }
  }
  return result;
}();

[[nodiscard]] std::span<const std::string_view>
arm_capabilities(const bool ros2_control) {
  return ros2_control
             ? std::span<const std::string_view>(kRosArmCapabilities)
             : std::span<const std::string_view>(kNativeArmCapabilities);
}

[[nodiscard]] std::string
component_type_id(const ActivationComponentProfile &profile) {
  return profile.model_asset_id +
         (profile.runtime_kind == ActivationRuntimeKind::kRos2Control
              ? ".fixed_scale.component"
              : ".component");
}
constexpr std::array<std::string_view, 14U> kCellCapabilities{{
    "collision.query.detailed",
    "collision.query.pre_execution",
    "observability.trace.scene",
    "scene.edit.add",
    "scene.edit.attach_detach",
    "scene.edit.remove",
    "scene.edit.update_pose",
    "scene.edit.upsert",
    "scene.object.query",
    "scene.reference.resolve",
    "scene.shape.box",
    "scene.shape.cylinder",
    "scene.shape.mesh",
    "scene.shape.sphere",
}};
// The generic native provider's lifecycle surface: exactly the intents its
// catalog plans. Recover is offered only for named supported conditions, and
// the native catalog currently has none.
constexpr std::array<std::string_view, 4U> kLifecycleCapabilities{{
    "component.lifecycle.disable", "component.lifecycle.enable",
    "component.lifecycle.observe", "component.lifecycle.prepare",
}};

[[noreturn]] void fail(std::string message) {
  throw std::invalid_argument("Activation Deployment compiler: " +
                              std::move(message));
}

[[nodiscard]] const CanonicalValue &member(const CanonicalValue &value,
                                           std::string_view name,
                                           std::string_view label) {
  const auto *result = value.find_object_member(name);
  if (result == nullptr) fail(std::string(label) + " is missing " + std::string(name));
  return *result;
}

[[nodiscard]] const Object &object(const CanonicalValue &value,
                                   std::string_view label) {
  const auto *result = std::get_if<Object>(&value.storage());
  if (result == nullptr) fail(std::string(label) + " must be an object");
  return *result;
}

[[nodiscard]] const Array &array(const CanonicalValue &value,
                                 std::string_view label) {
  const auto *result = std::get_if<Array>(&value.storage());
  if (result == nullptr) fail(std::string(label) + " must be an array");
  return *result;
}

[[nodiscard]] const std::string &text(const CanonicalValue &value,
                                      std::string_view label) {
  const auto *result = std::get_if<std::string>(&value.storage());
  if (result == nullptr) fail(std::string(label) + " must be text");
  return *result;
}


[[nodiscard]] CanonicalValue object_with(const CanonicalValue &source,
                                         std::string name,
                                         CanonicalValue selected) {
  Object result = object(source, "object");
  const auto found = std::find_if(result.begin(), result.end(), [&](const auto &row) {
    return row.first == name;
  });
  if (found == result.end()) {
    result.emplace_back(std::move(name), std::move(selected));
  } else {
    found->second = std::move(selected);
  }
  return CanonicalValue(std::move(result));
}

[[nodiscard]] CanonicalValue catalog_value(
    const core::CatalogRecordRefDTO &reference) {
  return std::visit(
      [](const auto &typed) {
        return CanonicalValue(Object{
            {"record_kind", CanonicalValue(std::string(core::to_string(
                                std::decay_t<decltype(typed)>::kRecordKind)))},
            {"record_id", CanonicalValue(typed.record_id.value())},
            {"record_revision", CanonicalValue(typed.record_revision.value())},
            {"schema_id", CanonicalValue(typed.schema_id.value())},
            {"schema_revision", CanonicalValue(typed.schema_revision.value())},
            {"content_digest", CanonicalValue(typed.content_digest.value())},
            {"content_addressed_store_key",
             CanonicalValue(typed.content_addressed_store_key.value())},
        });
      },
      reference);
}

class CatalogSelection final {
 public:
  explicit CatalogSelection(
      const std::vector<core::CatalogRecordRefDTO> &records) {
    for (const auto &record : records) {
      const auto key = std::pair{core::catalog_record_kind(record),
                                 core::catalog_record_id_value(record)};
      if (!records_.emplace(key, &record).second) {
        fail("qualified catalog repeats " + key.second);
      }
    }
  }

  [[nodiscard]] const core::CatalogRecordRefDTO &require(
      core::CatalogRecordKind kind, std::string_view id) {
    const auto found = records_.find({kind, std::string(id)});
    if (found == records_.end()) fail("qualified catalog lacks " + std::string(id));
    selected_.insert(found->second);
    return *found->second;
  }

  [[nodiscard]] CanonicalValue selected_value() const {
    std::vector<const core::CatalogRecordRefDTO *> rows(selected_.begin(),
                                                         selected_.end());
    std::sort(rows.begin(), rows.end(), [](const auto *left, const auto *right) {
      return core::CanonicalCatalogRecordRefLess{}(*left, *right);
    });
    Array result;
    result.reserve(rows.size());
    for (const auto *row : rows) result.push_back(catalog_value(*row));
    return CanonicalValue(Object{{"records", CanonicalValue(std::move(result))}});
  }

 private:
  std::map<std::pair<core::CatalogRecordKind, std::string>,
           const core::CatalogRecordRefDTO *> records_;
  std::set<const core::CatalogRecordRefDTO *> selected_;
};

[[nodiscard]] std::string revision(const core::CatalogRecordRefDTO &value) {
  return core::catalog_record_revision_value(value);
}

[[nodiscard]] CanonicalValue selector(CatalogSelection &catalog,
                                      std::string_view capability) {
  const auto &selected = catalog.require(
      core::CatalogRecordKind::kCapabilityDefinition, capability);
  return CanonicalValue(Object{
      {"capability_id", CanonicalValue(std::string(capability))},
      {"capability_definition_revision", CanonicalValue(revision(selected))},
  });
}

[[nodiscard]] std::vector<std::string>
source_joints(const ActivationComponentProfile &profile,
              std::string_view role) {
  const auto &semantics = member(profile.model_family, "semantics", "model family");
  for (const auto &binding : array(member(semantics, "resource_role_bindings",
                                          "model semantics"),
                                   "resource_role_bindings")) {
    if (text(member(binding, "resource_role_id", "resource binding"),
             "resource_role_id") != role) {
      continue;
    }
    std::vector<std::string> result;
    for (const auto &joint : array(member(binding, "source_joint_names",
                                           "resource binding"),
                                    "source_joint_names")) {
      result.push_back(text(joint, "source joint"));
    }
    return result;
  }
  return {};
}

[[nodiscard]] std::string binding_id(std::string_view component,
                                     std::string_view target,
                                     std::string_view capability) {
  std::string normalized(capability);
  std::replace(normalized.begin(), normalized.end(), '.', '-');
  return std::string(component) + "." + std::string(target) + "." + normalized;
}

[[nodiscard]] std::string domain_id(std::string_view component,
                                    std::string_view target) {
  return std::string(component) + "." + std::string(target) + ".control";
}

// The resource of the index-th source joint of a target's resource role.
[[nodiscard]] std::string coordinate_resource_id(std::string_view component,
                                                 std::string_view target,
                                                 std::size_t index) {
  std::ostringstream suffix;
  suffix << std::setw(6) << std::setfill('0') << index;
  return std::string(component) + "." + std::string(target) + ".coordinate." +
         suffix.str();
}

[[nodiscard]] CanonicalValue resource_bindings(
    std::string_view component, std::string_view target,
    const ActivationComponentProfile &profile) {
  const auto role = target == "arm" ? "arm_joints" : "fingers";
  const auto joints = source_joints(profile, role);
  Array result;
  for (std::size_t index = 0; index < joints.size(); ++index) {
    result.emplace_back(Object{
        {"resource_role_id", CanonicalValue(role)},
        {"resource_id", CanonicalValue(coordinate_resource_id(component, target, index))},
    });
  }
  if (result.empty() && target == "gripper") {
    result.emplace_back(Object{
        {"resource_role_id", CanonicalValue("fingers")},
        {"resource_id", CanonicalValue(std::string(component) +
                                         ".gripper.logical_position")},
    });
  }
  if (result.empty()) fail(std::string(component) + " has no arm resources");
  return CanonicalValue(std::move(result));
}

[[nodiscard]] CanonicalValue
motion_limits(const ActivationDeploymentComponent &component,
              const bool include_ordinary_jerk = true) {
  Array result;
  for (const auto &[target, settings] : object(component.target_settings,
                                                "target_settings")) {
    if (!include_ordinary_jerk &&
        settings.find_object_member("ordinary_motion_jerk") != nullptr) {
      fail("authored ROS ordinary motion jerk is not yet supported");
    }
    const auto *speed = settings.find_object_member("maximum_tcp_linear_speed_mps");
    if (speed == nullptr) continue;
    CanonicalValue jerk(Object{
        {"enabled", CanonicalValue(false)},
        {"joint_limits", CanonicalValue(Array{})},
        {"preset_reference_time_s", CanonicalValue(1.0)},
    });
    if (const auto *authored = settings.find_object_member("ordinary_motion_jerk");
        authored != nullptr) {
      jerk = *authored;
    }
    Object limit{
        {"command_target_id", CanonicalValue(target)},
        {"controllable_component_id", CanonicalValue(component.component_id)},
        {"maximum_tcp_linear_speed_mps", *speed},
    };
    if (include_ordinary_jerk) {
      limit.emplace_back("ordinary_motion_jerk", std::move(jerk));
    }
    result.emplace_back(std::move(limit));
  }
  return CanonicalValue(std::move(result));
}

struct ComponentProjection final {
  CanonicalValue runtime_configuration;
  CanonicalValue model_instance;
  CanonicalValue component;
  Array domains;
  Array bindings;
};

[[nodiscard]] ComponentProjection project_component(
    const ActivationDeploymentComponent &component,
    const ActivationComponentProfile &profile,
    const ActivationPreparedModel &model, CatalogSelection &catalog,
    std::string_view runtime_unit_id) {
  const bool ros2_control =
      profile.runtime_kind == ActivationRuntimeKind::kRos2Control;
  const std::string prefix = ros2_control ? "ros2_control" : "native";
  // Authored portable identifiers permit punctuation that cannot name the
  // existing Isaac controller namespace. Refuse it before runtime launch.
  if (ros2_control &&
      component.component_id.find_first_of(".-") != std::string::npos) {
    fail(component.component_id + " must be one ROS name token");
  }
  const auto &component_ref = catalog.require(
      core::CatalogRecordKind::kComponentType, component_type_id(profile));
  const auto &runtime_ref =
      catalog.require(core::CatalogRecordKind::kRuntimeProfile,
                      ros2_control ? "manyforge.ros2_control_execution_runtime"
                                   : "manyforge.native_execution_runtime");
  static_cast<void>(runtime_ref);
  static_cast<void>(catalog.require(core::CatalogRecordKind::kModelAsset,
                                    profile.model_asset_id));
  const auto &configuration = member(component.configuration,
                                     "runtime_configuration", "configuration");
  if (ros2_control) {
    const auto &profile_configuration =
        member(profile.runtime_restoration_profile, "ros2_control_runtime",
               "component profile");
    if (member(configuration, "isaac_profile", "runtime configuration") !=
        member(profile_configuration, "isaac_profile", "component profile")) {
      fail(component.component_id +
           " isaac_profile does not match its ROS component profile");
    }
  }
  const auto arm_resource_count =
      array(resource_bindings(component.component_id, "arm", profile),
            "arm resource bindings")
          .size();
  const auto &initial_joint_positions = array(
      member(configuration, "initial_joint_positions_rad",
             "runtime configuration"),
      "initial_joint_positions_rad");
  if (initial_joint_positions.size() != arm_resource_count) {
    fail(component.component_id +
         " initial_joint_positions_rad count does not match its "
         "arm resource bindings");
  }
  const auto limits = motion_limits(component, !ros2_control);
  if (ros2_control) {
    if (configuration.find_object_member("motion_target_limits") != nullptr) {
      fail("runtime_configuration.motion_target_limits must be authored "
           "through target_settings");
    }
    const auto &rows = array(limits, "motion limits");
    if (rows.size() != 1U ||
        text(member(rows.front(), "command_target_id", "motion limit"),
             "target") != "arm") {
      fail("authored ROS requires exactly one TCP-speed maximum for arm and no "
           "unknown target");
    }
  }
  CanonicalValue runtime_configuration =
      object_with(configuration, "motion_target_limits", limits);
  if (component.configuration.find_object_member(
          "native_lifecycle_management") != nullptr) {
    fail("native_lifecycle_management is obsolete; native component lifecycle "
         "is selected by the qualified provider product");
  }

  Array domains;
  Array bindings;
  for (const std::string_view target : {std::string_view("arm"),
                                        std::string_view("gripper")}) {
    const std::string contract_id = std::string("manyforge.") + prefix + "_" +
                                    std::string(target) + "_position";
    const std::string topology_id = std::string("manyforge.") + prefix + "_" +
                                    std::string(target) + "_direct";
    const auto &contract = catalog.require(
        core::CatalogRecordKind::kControlInterfaceContract, contract_id);
    const auto &topology = catalog.require(
        core::CatalogRecordKind::kControllerTopology, topology_id);
    const auto &stop = catalog.require(
        core::CatalogRecordKind::kStopEvidencePolicy,
        target == "arm" ? "manyforge.arm_motion_controlled_stop"
                        : "manyforge.native_controller_goal_terminal");
    const std::string provider_id = ros2_control
                                        ? "manyforge.provider.ros2_control"
                                        : "manyforge.provider.native_control";
    const auto &provider = catalog.require(
        core::CatalogRecordKind::kProviderManifest, provider_id);
    domains.emplace_back(Object{
        {"control_domain_id",
         CanonicalValue(domain_id(component.component_id, target))},
        {"control_domain_revision", CanonicalValue(std::string(kZeroDigest))},
        {"runtime_unit_ids",
         CanonicalValue(Array{CanonicalValue(std::string(runtime_unit_id))})},
        {"control_coordinator_endpoint_role",
         CanonicalValue(ros2_control ? "ros2_control_execution_status"
                                     : "native_execution_status")},
        {"controller_topology_id", CanonicalValue(topology_id)},
        {"topology_revision", CanonicalValue(revision(topology))},
        {"containment_granularity", CanonicalValue("resource")},
    });
    const std::span<const std::string_view> capabilities =
        target == "arm"
            ? arm_capabilities(ros2_control)
            : std::span<const std::string_view>(kGripperCapabilities);
    for (const auto capability : capabilities) {
      const auto capability_selector = selector(catalog, capability);
      const bool is_control =
          capability == "execution.contact.geometric" ||
          capability == "execution.contact.geometric_signed_penetration" ||
          capability.starts_with("execution.feedback.") ||
          capability == "execution.reset" ||
          capability == "execution.runtime_scale.operation_scoped" ||
          capability == "execution.stop.soft" ||
          capability == "execution.trajectory.execute" ||
          capability == "state.observed.current" ||
          capability.starts_with("tool.gripper.");
      if (!is_control) continue;
      Object binding{
          {"control_binding_id", CanonicalValue(binding_id(component.component_id,
                                                             target, capability))},
          {"control_binding_revision", CanonicalValue(std::string(kZeroDigest))},
          {"controllable_component_id", CanonicalValue(component.component_id)},
          {"command_target_id", CanonicalValue(std::string(target))},
      };
      for (const auto &row : object(capability_selector, "selector"))
        binding.push_back(row);
      binding.insert(
          binding.end(),
          {
              {"control_domain_id",
               CanonicalValue(domain_id(component.component_id, target))},
              {"resource_bindings",
               resource_bindings(component.component_id, target, profile)},
              {"control_interface_contract_id", CanonicalValue(contract_id)},
              {"control_interface_revision",
               CanonicalValue(revision(contract))},
              {"controller_topology_id", CanonicalValue(topology_id)},
              {"topology_revision", CanonicalValue(revision(topology))},
              {"legal_control_route_ids",
               CanonicalValue(
                   Array{CanonicalValue(std::string(target) + "_position")})},
              {"route_endpoint_projections",
               CanonicalValue(Array{CanonicalValue(Object{
                   {"control_route_id",
                    CanonicalValue(std::string(target) + "_position")},
                   {"command_endpoint_role_ids",
                    CanonicalValue(Array{CanonicalValue("command")})},
                   {"state_endpoint_role_ids",
                    CanonicalValue(Array{CanonicalValue("state")})},
               })})},
              // One selected execution provider serves this component's
              // domains.
              {"provider_instance_id",
               CanonicalValue(
                   component.component_id +
                   (ros2_control ? ".ros2_control" : ".native_control"))},
              {"provider_runtime_unit_id",
               CanonicalValue(std::string(runtime_unit_id))},
              {"provider_manifest_id", CanonicalValue(provider_id)},
              {"provider_manifest_revision",
               CanonicalValue(revision(provider))},
              {"state_authority_id",
               CanonicalValue(component.component_id + "." +
                              std::string(target) +
                              (ros2_control ? ".commanded_sim_state"
                                            : ".predicted_state"))},
              {"state_authority_class",
               CanonicalValue(ros2_control ? "commanded" : "predicted")},
              {"stop_evidence_policy_id",
               CanonicalValue(core::catalog_record_id_value(stop))},
              {"stop_evidence_policy_revision", CanonicalValue(revision(stop))},
              {"containment_granularity", CanonicalValue("resource")},
          });
      if (!ros2_control && target == "arm" &&
          (capability == "execution.trajectory.execute" ||
           capability == "execution.runtime_scale.operation_scoped")) {
        binding.emplace_back("runtime_scale_route_qualifications",
                             CanonicalValue(Array{CanonicalValue(Object{
                                 {"control_route_id", CanonicalValue("arm_position")},
                                 {"scaling_domain_id", CanonicalValue(component.component_id + ".arm.runtime_scale")},
                                 {"command_endpoint_role_id", CanonicalValue("command")},
                                 {"applied_state_endpoint_role_id", CanonicalValue("state")},
                                 {"acknowledgement_kind", CanonicalValue("applied_state")},
                                 {"maximum_slew_per_second", CanonicalValue(4.0)},
                                 {"freshness_budget_ns", CanonicalValue(100000000.0)},
                             })}));
      }
      bindings.emplace_back(std::move(binding));
    }
  }
  std::sort(bindings.begin(), bindings.end(), [](const auto &left, const auto &right) {
    return text(member(left, "control_binding_id", "binding"), "binding id") <
           text(member(right, "control_binding_id", "binding"), "binding id");
  });
  return ComponentProjection{
      std::move(runtime_configuration),
      CanonicalValue(Object{
          {"model_instance_id",
           CanonicalValue(component.component_id + ".model")},
          {"model_asset_id", CanonicalValue(profile.model_asset_id)},
          {"model_revision",
           CanonicalValue(model.catalog_reference.record_revision.value())},
      }),
      CanonicalValue(Object{
          {"controllable_component_id", CanonicalValue(component.component_id)},
          {"component_type_id", CanonicalValue(component_type_id(profile))},
          {"component_type_revision", CanonicalValue(revision(component_ref))},
          {"model_role_bindings",
           CanonicalValue(Array{CanonicalValue(Object{
               {"model_role_id", CanonicalValue("arm_model")},
               {"model_instance_id",
                CanonicalValue(component.component_id + ".model")},
           })})},
          {"runtime_unit_ids",
           CanonicalValue(Array{CanonicalValue(std::string(runtime_unit_id))})},
          {"root_pose_in_cell", component.root_pose_in_cell},
      }),
      std::move(domains), std::move(bindings)};
}

[[nodiscard]] const CanonicalValue &find_record(const Array &rows,
                                                std::string_view field,
                                                std::string_view value) {
  const auto found = std::find_if(rows.begin(), rows.end(), [&](const auto &row) {
    return text(member(row, field, "record"), field) == value;
  });
  if (found == rows.end()) fail("computed graph lacks " + std::string(value));
  return *found;
}

[[nodiscard]] std::string graph_revision(const CanonicalValue &graph,
                                         std::string_view collection,
                                         std::string_view id_field,
                                         std::string_view id,
                                         std::string_view revision_field) {
  return text(member(find_record(array(member(graph, collection, "graph"), collection),
                                 id_field, id),
                     revision_field, "record"), revision_field);
}

[[nodiscard]] CanonicalValue provider_contract_ref(bool hard) {
  return CanonicalValue(Object{
      {"provider_contract_role_id", CanonicalValue(hard ? "software_hard_stop" : "stop_contract")},
      {"contract_schema_id", CanonicalValue(hard
          ? "manyforge.value_schema.provider_contract.empty.v1"
          : "manyforge.value_schema.provider_contract.motion_controlled_stop.v1")},
      {"contract_schema_revision", CanonicalValue(hard
          ? "sha256:fb466dd341076cbdb6fc97558ad28bc46032d25daaf4d4bb9d548bf642b83270"
          : "sha256:58ec6382a268329e9d8001eb53aac31259b5eb95a3690095aa4bbe12ac51d83b")},
  });
}

} // namespace

CanonicalValue compile_activation_deployment(
    const ActivationDeploymentIntent &intent,
    const core::CellId &runtime_cell_id,
    const std::vector<ActivationComponentProfile> &component_profiles,
    const std::vector<ActivationPreparedModel> &prepared_models,
    const std::vector<core::CatalogRecordRefDTO> &catalog_records,
    const std::vector<QualifiedRuntimeProfile> &runtime_profiles) {
  if (intent.components.size() != component_profiles.size() ||
      intent.components.size() != prepared_models.size()) {
    fail("component/profile/model joins differ in size");
  }
  if (!array(intent.attachments, "attachments").empty()) {
    fail("component attachments are not yet supported by the authored compiler");
  }
  CatalogSelection catalog(catalog_records);
  const bool ros2_control =
      !component_profiles.empty() && component_profiles.front().runtime_kind ==
                                         ActivationRuntimeKind::kRos2Control;
  if (std::any_of(component_profiles.begin(), component_profiles.end(),
                  [&](const auto &profile) {
                    return (profile.runtime_kind ==
                            ActivationRuntimeKind::kRos2Control) !=
                           ros2_control;
                  })) {
    fail("mixed native and ROS runtime compositions are not yet supported");
  }
  if (ros2_control && (component_profiles.size() != 1U ||
                       component_profiles.front().component_profile_id !=
                           "ur10e_robotiq_isaac_commanded_v0")) {
    fail("authored ROS composition requires one UR10e aggregate profile");
  }
  const std::string runtime_profile_id =
      ros2_control
          ? std::string(
                ros2_control_execution_runtime_contract::kRuntimeProfileId)
          : std::string(native_execution_runtime_contract::kRuntimeProfileId);
  const auto &runtime_ref = catalog.require(
      core::CatalogRecordKind::kRuntimeProfile, runtime_profile_id);
  std::vector<QualifiedRuntimeProfile> selected_profiles;
  for (const auto &profile : runtime_profiles) {
    if (profile.value().runtime_profile_id.value() == runtime_profile_id) {
      selected_profiles.push_back(profile);
    }
  }
  if (selected_profiles.size() != 1U)
    fail("execution runtime profile selection is not exact");
  if (intent.components.empty()) {
    if (!object(intent.policies, "policies").empty() || intent.assistant_profile_id.has_value()) {
      fail("empty Deployment has no component policies or assistant profile");
    }
    auto authored = native_execution_runtime_contract::empty_host_deployment(
        runtime_cell_id, catalog_value(runtime_ref), intent.deployment_artifact_id, intent.name).as_object();
    authored.emplace_back("compatibility", CanonicalValue(Object{
        {"lane", CanonicalValue("manyforge-only")},
        {"provides", CanonicalValue(Array{})},
        {"tags", CanonicalValue(Array{})},
    }));
    return compute_deployment_artifact_revision(
        parse_strict_json_model_yaml(canonical_json(CanonicalValue(std::move(authored)))), selected_profiles).normalized_authored_root();
  }
  const std::string runtime_id =
      ros2_control
          ? intent.components.front().component_id + ".ros2_control_execution"
          : std::string(
                native_execution_runtime_contract::kCellHostRuntimeUnitId);
  std::vector<ComponentProjection> projections;
  projections.reserve(intent.components.size());
  for (std::size_t index = 0; index < intent.components.size(); ++index) {
    projections.push_back(project_component(
        intent.components[index], component_profiles[index],
        prepared_models[index], catalog, runtime_id));
  }

  CanonicalValue runtime_configuration = projections.front().runtime_configuration;
  if (projections.size() > 1U) {
    for (const std::string_view name : {"planner_rng_seed",
                                        "execution_tick_hz",
                                        "collision_monitor_period_ms", "mode"}) {
      const auto &expected = member(runtime_configuration, name, "runtime configuration");
      for (const auto &projection : projections) {
        if (member(projection.runtime_configuration, name, "runtime configuration") !=
            expected) {
          fail("grouped native components disagree on " + std::string(name));
        }
      }
    }
    for (const std::string_view name : {"initial_joint_positions_rad",
                                        "motion_target_limits",
                                        "direct_control_joint_limits",
                                        "gripper_target_configurations"}) {
      Array combined;
      for (const auto &projection : projections) {
        const auto *selected = projection.runtime_configuration.find_object_member(name);
        if (selected == nullptr) continue;
        const auto &values = array(*selected, name);
        combined.insert(combined.end(), values.begin(), values.end());
      }
      if (name != "initial_joint_positions_rad") {
        std::sort(combined.begin(), combined.end(), [](const auto &left, const auto &right) {
          const auto key = [](const auto &row) {
            const auto *component = row.find_object_member("controllable_component_id");
            const auto *target = row.find_object_member("command_target_id");
            const auto *joint = row.find_object_member("source_joint_name");
            return std::tuple(component == nullptr ? "" : component->as_string(),
                              target == nullptr ? "" : target->as_string(),
                              joint == nullptr ? "" : joint->as_string());
          };
          return key(left) < key(right);
        });
      }
      runtime_configuration = object_with(runtime_configuration, std::string(name),
                                          CanonicalValue(std::move(combined)));
    }
  }

  Array models;
  Array components;
  Array domains;
  Array bindings;
  for (auto &projection : projections) {
    models.push_back(std::move(projection.model_instance));
    components.push_back(std::move(projection.component));
    domains.insert(domains.end(), projection.domains.begin(), projection.domains.end());
    bindings.insert(bindings.end(), projection.bindings.begin(), projection.bindings.end());
  }
  const auto by = [](std::string_view field) {
    return [field](const auto &left, const auto &right) {
      return text(member(left, field, "record"), field) <
             text(member(right, field, "record"), field);
    };
  };
  std::sort(models.begin(), models.end(), by("model_instance_id"));
  std::sort(components.begin(), components.end(), by("controllable_component_id"));
  std::sort(domains.begin(), domains.end(), by("control_domain_id"));
  std::sort(bindings.begin(), bindings.end(), by("control_binding_id"));

  CanonicalValue graph(Object{
      {"api_version", CanonicalValue(std::string(core::kCellGraphApiVersion))},
      {"graph_revision", CanonicalValue(std::string(kZeroDigest))},
      {"cell_id", CanonicalValue(runtime_cell_id.value())},
      {"runtime_units",
       CanonicalValue(Array{CanonicalValue(Object{
           {"runtime_unit_id", CanonicalValue(runtime_id)},
           {"runtime_profile_id", CanonicalValue(runtime_profile_id)},
           {"runtime_profile_revision", CanonicalValue(revision(runtime_ref))},
           {"dependency_runtime_unit_ids", CanonicalValue(Array{})},
           {"configuration", std::move(runtime_configuration)},
       })})},
      {"model_instances", CanonicalValue(std::move(models))},
      {"controllable_components", CanonicalValue(std::move(components))},
      {"control_domains", CanonicalValue(std::move(domains))},
      {"control_bindings", CanonicalValue(std::move(bindings))},
      {"component_attachments", CanonicalValue(Array{})},
  });
  const auto graph_parsed = parse_strict_json_model_yaml(canonical_json(graph));
  const auto typed = decode_cell_graph_candidate(graph_parsed);
  const auto intrinsic = validate_intrinsic_cell_graph_candidate(typed);
  // The native provider composes one controllable component per domain;
  // explain that limitation at compile time rather than at activation.
  if (!ros2_control) {
    if (const auto topology =
            VerifySupportedNativeDomainTopology(intrinsic.graph());
        !topology.ok()) {
      fail(topology.error().message);
    }
  }
  const auto ready = prepare_cell_graph_identity(intrinsic, selected_profiles);
  const auto computed = compute_graph_revision(ready);
  const auto &computed_graph = computed.normalized_root();

  Array desired;
  Array providers;
  Array stop_stages;
  Array stop_timing;
  for (std::size_t index = 0; index < intent.components.size(); ++index) {
    const auto &component = intent.components[index];
    const auto &profile = component_profiles[index];
    for (const std::string_view target : {std::string_view("arm"),
                                          std::string_view("gripper")}) {
      const std::string topology_id =
          std::string("manyforge.") +
          (ros2_control ? "ros2_control_" : "native_") + std::string(target) +
          "_direct";
      const auto &topology = catalog.require(
          core::CatalogRecordKind::kControllerTopology, topology_id);
      const std::string domain = domain_id(component.component_id, target);
      const std::span<const std::string_view> capabilities =
          target == "arm"
              ? arm_capabilities(ros2_control)
              : std::span<const std::string_view>(kGripperCapabilities);
      Array routes;
      for (const auto capability : capabilities) {
        const std::string id = binding_id(component.component_id, target, capability);
        const auto &computed_bindings_for_routes =
            array(member(computed_graph, "control_bindings", "graph"), "bindings");
        const auto found = std::find_if(
            computed_bindings_for_routes.begin(),
            computed_bindings_for_routes.end(),
            [&](const auto &row) {
              return text(member(row, "control_binding_id", "binding"), "id") == id;
            });
        if (found != computed_bindings_for_routes.end()) {
          routes.emplace_back(Object{
              {"control_binding_id", CanonicalValue(id)},
              {"control_binding_revision", CanonicalValue(text(
                  member(*found, "control_binding_revision", "binding"), "revision"))},
              {"control_route_id", CanonicalValue(std::string(target) + "_position")},
          });
        }
      }
      desired.emplace_back(Object{
          {"control_domain_id", CanonicalValue(domain)},
          {"control_domain_revision", CanonicalValue(graph_revision(
              computed_graph, "control_domains", "control_domain_id", domain,
              "control_domain_revision"))},
          {"controller_topology_id", CanonicalValue(topology_id)},
          {"topology_revision", CanonicalValue(revision(topology))},
          {"activation_set_id", CanonicalValue(std::string(target) + "_direct")},
          {"binding_routes", CanonicalValue(std::move(routes))},
      });

      const std::string anchor_capability =
          target == "arm" ? "execution.trajectory.execute" : "tool.gripper.command";
      const std::string anchor = binding_id(component.component_id, target,
                                            anchor_capability);
      const std::string anchor_revision = graph_revision(
          computed_graph, "control_bindings", "control_binding_id", anchor,
          "control_binding_revision");
      const auto &component_ref = catalog.require(
          core::CatalogRecordKind::kComponentType, component_type_id(profile));
      const auto &planning =
          catalog.require(core::CatalogRecordKind::kProviderManifest,
                          ros2_control ? "manyforge.provider.ros2_control"
                                       : "manyforge.provider.native_planning");
      for (const auto capability : capabilities) {
        const bool is_provider = capability.starts_with("model.") ||
                                 capability.starts_with("planning.") ||
                                 capability.starts_with("validation.");
        if (!is_provider) continue;
        std::string normalized(capability);
        std::replace(normalized.begin(), normalized.end(), '.', '-');
        Object row{
            {"capability_provider_binding_id", CanonicalValue(
                component.component_id + "." + std::string(target) + "." +
                normalized + ".provider")},
            {"capability_provider_binding_revision", CanonicalValue(std::string(kZeroDigest))},
        };
        const auto selected_capability = selector(catalog, capability);
        for (const auto &selected :
             object(selected_capability, "selector")) {
          row.push_back(selected);
        }
        row.insert(
            row.end(),
            {
                {"scope", CanonicalValue("target_binding")},
                {"target",
                 CanonicalValue(Object{
                     {"controllable_component_id",
                      CanonicalValue(component.component_id)},
                     {"component_type_revision",
                      CanonicalValue(revision(component_ref))},
                     {"command_target_id", CanonicalValue(std::string(target))},
                     {"control_binding_id", CanonicalValue(anchor)},
                     {"control_binding_revision",
                      CanonicalValue(anchor_revision)},
                 })},
                {"provider_instance_id",
                 CanonicalValue(
                     component.component_id +
                     (ros2_control ? ".ros2_control" : ".native_planning"))},
                {"provider_manifest_id",
                 CanonicalValue(ros2_control
                                    ? "manyforge.provider.ros2_control"
                                    : "manyforge.provider.native_planning")},
                {"provider_manifest_revision",
                 CanonicalValue(revision(planning))},
                {"endpoint_bindings",
                 CanonicalValue(Array{CanonicalValue(Object{
                     {"endpoint_role_id",
                      CanonicalValue("planner_scene_projection")},
                     {"source_kind", CanonicalValue("runtime_unit")},
                     {"source_id", CanonicalValue(runtime_id)},
                     {"source_endpoint_role_id",
                      CanonicalValue("planner_scene_projection")},
                 })})},
            });
        providers.emplace_back(std::move(row));
      }

      const auto &computed_bindings =
          array(member(computed_graph, "control_bindings", "graph"), "bindings");
      for (const auto &binding : computed_bindings) {
        if (text(member(binding, "controllable_component_id", "binding"), "component") !=
                component.component_id ||
            text(member(binding, "command_target_id", "binding"), "target") != target) {
          continue;
        }
        const auto stage = [&](std::string_view stage_id, bool hard,
                               CanonicalValue parameters) {
          stop_stages.emplace_back(Object{
              {"control_domain_id", member(binding, "control_domain_id", "binding")},
              {"control_domain_revision", CanonicalValue(graph_revision(
                  computed_graph, "control_domains", "control_domain_id", domain,
                  "control_domain_revision"))},
              {"control_binding_id", member(binding, "control_binding_id", "binding")},
              {"control_binding_revision", member(binding, "control_binding_revision", "binding")},
              {"controller_topology_id", member(binding, "controller_topology_id", "binding")},
              {"topology_revision", member(binding, "topology_revision", "binding")},
              {"control_route_id", CanonicalValue(std::string(target) + "_position")},
              {"stop_evidence_policy_id", member(binding, "stop_evidence_policy_id", "binding")},
              {"stop_evidence_policy_revision", member(binding, "stop_evidence_policy_revision", "binding")},
              {"stop_escalation_stage_id", CanonicalValue(std::string(stage_id))},
              {"provider_contract_ref", provider_contract_ref(hard)},
              {"default_contract_parameters", std::move(parameters)},
          });
        };
        if (target == "arm") {
          const auto &settings = member(component.target_settings, "arm", "target settings");
          const auto &controlled = member(settings, "controlled_stop", "arm settings");
          // Authored rows name their source joint; pair them with the arm
          // resources by that name, never by row position.
          const auto joints = source_joints(profile, "arm_joints");
          std::map<std::string, const CanonicalValue *, std::less<>> limit_by_joint;
          for (const auto &row : array(member(controlled, "per_joint_limits", "controlled stop"),
                                       "per_joint_limits")) {
            const auto *name = row.find_object_member("source_joint_name");
            if (name == nullptr) fail("controlled Stop limit row lacks source_joint_name");
            const auto &joint = text(*name, "source_joint_name");
            if (joint.empty()) fail("controlled Stop limit row lacks source_joint_name");
            if (std::find(joints.begin(), joints.end(), joint) == joints.end()) {
              fail("controlled Stop limits name unknown arm joint " + joint);
            }
            if (!limit_by_joint.emplace(joint, &row).second) {
              fail("controlled Stop limits repeat joint " + joint);
            }
          }
          Array resource_limits;
          for (const auto &resource : array(member(binding, "resource_bindings", "binding"),
                                            "resource bindings")) {
            const auto &resource_id = text(member(resource, "resource_id", "resource"),
                                           "resource_id");
            std::size_t index = 0;
            while (index < joints.size() &&
                   coordinate_resource_id(component.component_id, target, index) != resource_id) {
              ++index;
            }
            if (index == joints.size()) fail("arm resource " + resource_id + " has no source joint");
            const auto found = limit_by_joint.find(joints[index]);
            if (found == limit_by_joint.end()) {
              fail("controlled Stop limits miss joint " + joints[index]);
            }
            const auto &limit = *found->second;
            Object resource_limit{
                {"resource_id", CanonicalValue(resource_id)},
                {"max_deceleration", CanonicalValue(Object{
                    {"coordinate_kind", CanonicalValue("angular_rad_s2")},
                    {"value_rad_s2", member(limit, "max_deceleration_rad_s2", "limit")},
                })},
                {"settle_velocity_threshold", CanonicalValue(Object{
                    {"coordinate_kind", CanonicalValue("angular_rad_s")},
                    {"value_rad_s", member(limit, "settle_velocity_threshold_rad_s", "limit")},
                })},
            };
            // An authored jerk limit replaces the derived preset for this
            // joint; it is never dropped. Rows without it compile unchanged.
            if (const auto *jerk = limit.find_object_member("max_jerk_rad_s3");
                jerk != nullptr) {
              const auto *value = std::get_if<double>(&jerk->storage());
              if (value == nullptr || !std::isfinite(*value) || *value <= 0.0) {
                fail("controlled Stop max_jerk_rad_s3 for joint " + joints[index] +
                     " must be a finite positive number");
              }
              resource_limit.emplace_back(
                  "max_jerk", CanonicalValue(Object{
                                  {"coordinate_kind", CanonicalValue("angular_rad_s3")},
                                  {"value_rad_s3", CanonicalValue(*value)},
                              }));
            }
            resource_limits.emplace_back(std::move(resource_limit));
          }
          stage("motion_controlled_stop", false, CanonicalValue(Object{
              {"stop_mode", member(controlled, "stop_mode", "controlled stop")},
              {"jerk_limits_enabled", member(controlled, "jerk_limits_enabled", "controlled stop")},
              {"deceleration_time_s", member(controlled, "deceleration_time_s", "controlled stop")},
              {"per_resource_limits", CanonicalValue(std::move(resource_limits))},
              {"allow_natural_completion_on_short_path", member(controlled, "allow_natural_completion_on_short_path", "controlled stop")},
          }));
        }
        stage("software_hard", true, CanonicalValue(Object{}));
        const auto &settings = member(component.target_settings, "arm", "target settings");
        const auto &timing = member(member(settings, "controlled_stop", "arm settings"),
                                    "timing", "controlled stop");
        stop_timing.emplace_back(Object{
            {"control_binding_id", member(binding, "control_binding_id", "binding")},
            {"control_binding_revision", member(binding, "control_binding_revision", "binding")},
            {"timing", timing},
        });
      }
    }
    // Every native component uses the generic CCLC lifecycle provider.
    for (const auto capability : kLifecycleCapabilities) {
      static_cast<void>(selector(catalog, capability));
    }
    if (ros2_control) {
      static_cast<void>(selector(catalog, "component.lifecycle.recover"));
    }
  }
  const auto &logical = catalog.require(core::CatalogRecordKind::kProviderManifest,
                                        "manyforge.provider.logical_scene");
  for (const auto capability : kCellCapabilities) {
    std::string normalized(capability);
    std::replace(normalized.begin(), normalized.end(), '.', '-');
    Object row{
        {"capability_provider_binding_id", CanonicalValue(
            runtime_cell_id.value() + "." + normalized + ".provider")},
        {"capability_provider_binding_revision", CanonicalValue(std::string(kZeroDigest))},
    };
    const auto selected_capability = selector(catalog, capability);
    for (const auto &selected : object(selected_capability, "selector")) {
      row.push_back(selected);
    }
    row.insert(row.end(), {
        {"scope", CanonicalValue("cell_global")},
        {"target", CanonicalValue(Object{{"cell_id", CanonicalValue(runtime_cell_id.value())}})},
        {"provider_instance_id", CanonicalValue(runtime_cell_id.value() + ".logical_scene")},
        {"provider_manifest_id", CanonicalValue("manyforge.provider.logical_scene")},
        {"provider_manifest_revision", CanonicalValue(revision(logical))},
        {"endpoint_bindings", CanonicalValue(Array{CanonicalValue(Object{
            {"endpoint_role_id", CanonicalValue("logical_scene_services")},
            {"source_kind", CanonicalValue("runtime_unit")},
            {"source_id", CanonicalValue(runtime_id)},
            {"source_endpoint_role_id", CanonicalValue("logical_scene_services")},
        })})},
    });
    providers.emplace_back(std::move(row));
  }
  static_cast<void>(catalog.require(core::CatalogRecordKind::kCommandSchema,
                                    "gripper.command_position"));
  std::sort(desired.begin(), desired.end(), by("control_domain_id"));
  std::sort(providers.begin(), providers.end(), by("capability_provider_binding_id"));
  std::sort(stop_stages.begin(), stop_stages.end(), [](const auto &left, const auto &right) {
    return std::tuple(text(member(left, "control_binding_id", "stage"), "id"),
                      text(member(left, "stop_escalation_stage_id", "stage"), "stage")) <
           std::tuple(text(member(right, "control_binding_id", "stage"), "id"),
                      text(member(right, "stop_escalation_stage_id", "stage"), "stage"));
  });
  std::sort(stop_timing.begin(), stop_timing.end(), by("control_binding_id"));

  Object policies{
      {"retained_policy_records", CanonicalValue(Array{})},
      {"stop_stage_parameters", CanonicalValue(std::move(stop_stages))},
      {"stop_timing_parameters", CanonicalValue(std::move(stop_timing))},
      {"commissioned_stop_interfaces", CanonicalValue(Array{})},
      {"interrupt_sources", CanonicalValue(Array{})},
      {"interrupt_policies", CanonicalValue(Array{})},
      {"interrupt_config", member(intent.policies, "interrupt_config", "policies")},
  };
  Object authored_graph;
  for (const std::string_view name : {"runtime_units", "model_instances",
                                      "controllable_components", "control_domains",
                                      "control_bindings", "component_attachments"}) {
    authored_graph.emplace_back(std::string(name), member(computed_graph, name, "graph"));
  }
  Object root{
      {"api_version", CanonicalValue("manyforge.deployment.v1")},
      {"deployment_artifact_id", CanonicalValue(intent.deployment_artifact_id)},
      {"deployment_revision", CanonicalValue(std::string(kZeroDigest))},
      {"cell_id", CanonicalValue(runtime_cell_id.value())},
      {"name", CanonicalValue(intent.name)},
      {"catalogs", catalog.selected_value()},
      {"graph", CanonicalValue(std::move(authored_graph))},
      {"desired_control_configurations", CanonicalValue(std::move(desired))},
      {"capability_provider_bindings", CanonicalValue(std::move(providers))},
      {"policies", CanonicalValue(std::move(policies))},
  };
  if (intent.description.has_value())
    root.emplace_back("description", CanonicalValue(*intent.description));
  if (intent.compatibility.has_value())
    root.emplace_back("compatibility", *intent.compatibility);
  if (intent.assistant_profile_id.has_value())
    fail("assistant profiles are not yet supported by the authored compiler");
  const auto candidate = parse_strict_json_model_yaml(
      canonical_json(CanonicalValue(std::move(root))));
  return compute_deployment_artifact_revision(candidate, selected_profiles)
      .normalized_authored_root();
}

} // namespace manyforge::deployment::detail
