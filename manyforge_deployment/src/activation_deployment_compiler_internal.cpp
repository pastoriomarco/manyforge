#include "activation_deployment_compiler_internal.hpp"
#include "native_execution_runtime_unit_internal.hpp"
#include "ros2_control_execution_runtime_unit_internal.hpp"
#include "stop_deceleration_coherence_internal.hpp"
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/io_provider_lifecycle_catalog.hpp>
#include <manyforge_deployment/io_provider_domain.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <manyforge_core/types/cell_graph.hpp>
#include <manyforge_core/types/command_schema.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/cell_graph_codec.hpp>
#include <manyforge_deployment/deployment_artifact_codec.hpp>
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>

namespace manyforge::deployment::detail {
namespace {

using Array = CanonicalValue::Array;
using Object = CanonicalValue::Object;

constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";

constexpr std::array<std::string_view, 18U> kRosArmCapabilities{{
    "execution.contact.geometric",
    "execution.contact.geometric_signed_penetration",
    "execution.feedback.phase_events",
    "execution.feedback.state_snapshots",
    "execution.reset",
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
[[nodiscard]] std::span<const std::string_view>
arm_capabilities() {
  return kRosArmCapabilities;
}

[[nodiscard]] std::string
component_type_id(const ActivationComponentProfile &profile,
                  const bool has_gripper) {
  if (is_model_free_runtime_kind(profile.runtime_kind))
    throw std::logic_error(
        "model-free type must come from its actual qualified structural "
        "artifact");
  // A Real arm without an authored gripper remains arm-only. An authored Real
  // pneumatic gripper uses the same model-backed target as the Mock component.
  const auto *ros = profile.runtime_restoration_profile.find_object_member(
      "ros2_control_runtime");
  if (ros != nullptr && ros->find_object_member("real_profile") != nullptr &&
      !has_gripper)
    return profile.model_asset_id + ".fixed_scale.arm_only.component";
  return profile.model_asset_id + ".fixed_scale.component";
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

[[nodiscard]] CanonicalValue object_without(const CanonicalValue &source,
                                            std::string_view name) {
  Object result = object(source, "object");
  std::erase_if(result, [&](const auto &row) { return row.first == name; });
  return CanonicalValue(std::move(result));
}

// Q68: the controller profile a ROS component profile selects from the
// catalog: exactly one of `isaac_profile` (Isaac lane), `mock_profile`
// (offline mock lane) or, temporarily until CP8 step 3, `real_profile` (a
// real robot). Returns the selector field name, or none.
[[nodiscard]] std::optional<std::string_view>
controller_profile_selector(const CanonicalValue &ros2_control_runtime) {
  std::optional<std::string_view> selected;
  for (const std::string_view name : {std::string_view("isaac_profile"),
                                      std::string_view("mock_profile"),
                                      std::string_view("real_profile")}) {
    const auto *value = ros2_control_runtime.find_object_member(name);
    if (value == nullptr) continue;
    if (!value->is_string() || value->as_string().empty() ||
        selected.has_value()) {
      return std::nullopt;
    }
    selected = name;
  }
  return selected;
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
  if (is_model_free_runtime_kind(profile.runtime_kind))
    throw std::logic_error("model-free profile has no physical model source");
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

// CP8 step 1: a real robot reports measured state; Mock and Isaac stay
// commanded (CP4 authority rule).
[[nodiscard]] bool real_controller_profile(
    const ActivationComponentProfile &profile) {
  if (profile.runtime_kind != ActivationRuntimeKind::kRos2Control) return false;
  const auto *runtime =
      profile.runtime_restoration_profile.find_object_member("ros2_control_runtime");
  return runtime != nullptr &&
         controller_profile_selector(*runtime) == std::string_view("real_profile");
}

// R7 package 4: a Real component's ros2_control provider carries the Real
// lifecycle catalog (arming at Prepare, D4 Disable, D10 Recover); it offers
// what the shared provider offers. Mock and Isaac keep the shared provider.
// Every Real profile is xArm-family today (`lite6_real`); another vendor's
// row adds its own provider here. A Real gripper domain has its own provider:
// the shared gripper plans, with Recover aligned to the arm's `disabled`
// target (one common Recover target, spec 210 section 3.17).
[[nodiscard]] std::string ros2_control_provider_id(
    const ActivationComponentProfile &profile, std::string_view target) {
  if (!real_controller_profile(profile)) return "manyforge.provider.ros2_control";
  return target == "gripper" ? "manyforge.provider.ros2_control.xarm.gripper"
                             : "manyforge.provider.ros2_control.xarm";
}

// One provider instance selects one manifest pair (spec 240), so a Real
// gripper's provider is its own instance; Mock and Isaac keep one instance.
[[nodiscard]] std::string ros2_control_provider_instance_id(
    const ActivationDeploymentComponent &component,
    const ActivationComponentProfile &profile, std::string_view target) {
  return component.component_id + ".ros2_control" +
         (real_controller_profile(profile) && target == "gripper" ? ".gripper" : "");
}

// Do not infer a gripper from passive Real model geometry. A Real gripper is
// present only when the component explicitly authors its pneumatic target.
[[nodiscard]] bool has_gripper_target(const ActivationDeploymentComponent &component,
                                      const ActivationComponentProfile &profile) {
  if (source_joints(profile, "fingers").empty()) return false;
  if (!real_controller_profile(profile)) return true;
  const auto *configuration =
      component.configuration.find_object_member("runtime_configuration");
  const auto *grippers = configuration == nullptr
                             ? nullptr
                             : configuration->find_object_member("gripper_target_configurations");
  if (grippers == nullptr) return false;
  for (const auto &row : array(*grippers, "gripper_target_configurations")) {
    if (text(member(row, "controllable_component_id", "gripper target"),
             "gripper component") == component.component_id &&
        text(member(row, "command_target_id", "gripper target"), "gripper target") ==
            "gripper") {
      if (text(member(row, "actuation_kind", "gripper target"), "gripper actuation") !=
          "pneumatic")
        fail(component.component_id + " Real gripper must be pneumatic");
      if (text(member(row, "output_mode", "gripper target"), "gripper output") ==
              "commanded" && row.find_object_member("pneumatic_io") == nullptr)
        fail("real_pneumatic_io_not_wired: " + component.component_id +
             " commanded pneumatic I/O is not yet wired");
      return true;
    }
  }
  return false;
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
  if (result.empty()) fail(std::string(component) + " has no " +
                           std::string(target) + " resources");
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

// Q37: carry the component's authored `default_fault_stop_profile` (main's
// field set) unchanged into the ROS runtime configuration for its arm target.
// The source-joint map becomes canonical-sorted rows because the closed
// runtime schema has no map form; nothing is derived or defaulted here.
[[nodiscard]] CanonicalValue fault_stop_profile_row(
    const ActivationDeploymentComponent &component,
    const CanonicalValue &authored) {
  constexpr std::array<std::string_view, 6U> kFields{
      "profile_id",          "stop_mode",
      "deceleration_time_s", "per_joint_max_deceleration_rad_s2",
      "allow_natural_completion_on_short_path",
      "require_backend_confirmation"};
  Object row{
      {"controllable_component_id", CanonicalValue(component.component_id)},
      {"command_target_id", CanonicalValue("arm")},
  };
  for (const auto &[name, value] :
       object(authored, "configuration.default_fault_stop_profile")) {
    if (std::find(kFields.begin(), kFields.end(), name) == kFields.end()) {
      fail(component.component_id + " default_fault_stop_profile field " +
           name + " is not supported");
    }
    if (name != "per_joint_max_deceleration_rad_s2") {
      row.emplace_back(name, value);
      continue;
    }
    Array limits;
    for (const auto &[joint, limit] :
         object(value, "default_fault_stop_profile.per_joint_max_deceleration_rad_s2")) {
      limits.push_back(CanonicalValue(Object{
          {"source_joint_name", CanonicalValue(joint)},
          {"max_deceleration_rad_s2", limit},
      }));
    }
    std::sort(limits.begin(), limits.end(), [](const auto &left, const auto &right) {
      return text(member(left, "source_joint_name", "row"), "joint") <
             text(member(right, "source_joint_name", "row"), "joint");
    });
    row.emplace_back(name, CanonicalValue(std::move(limits)));
  }
  return CanonicalValue(std::move(row));
}

struct ComponentProjection final {
  CanonicalValue runtime_configuration;
  CanonicalValue model_instance;
  CanonicalValue component;
  Array domains;
  Array bindings;
};

[[nodiscard]] std::string_view gripper_stop_policy_id(
    const ActivationDeploymentComponent &component,
    const CanonicalValue &configuration) {
  const CanonicalValue *selected = nullptr;
  for (const auto &row : array(member(configuration, "gripper_target_configurations",
                                      "runtime configuration"),
                                "gripper_target_configurations")) {
    if (text(member(row, "controllable_component_id", "gripper target"),
             "gripper component") != component.component_id ||
        text(member(row, "command_target_id", "gripper target"),
             "gripper target") != "gripper") {
      continue;
    }
    if (selected != nullptr) {
      fail(component.component_id + " repeats its gripper target configuration");
    }
    selected = &row;
  }
  if (selected == nullptr) {
    fail(component.component_id + " has no exact authored gripper target configuration");
  }
  const auto &fields = object(*selected, "gripper target configuration");
  const auto &actuation = text(member(*selected, "actuation_kind", "gripper target"),
                               "gripper actuation kind");
  const auto *duration = std::get_if<double>(
      &member(*selected, "command_duration_s", "gripper target").storage());
  const auto &output = text(member(*selected, "output_mode", "gripper target"),
                            "gripper output mode");
  const auto *timeout = selected->find_object_member("delivery_result_timeout_s");
  const auto *pneumatic_io = selected->find_object_member("pneumatic_io");
  const auto *timeout_seconds =
      timeout == nullptr ? nullptr : std::get_if<double>(&timeout->storage());
  if (fields.size() != 13U + static_cast<std::size_t>(timeout != nullptr) +
                           static_cast<std::size_t>(pneumatic_io != nullptr) || duration == nullptr ||
      !execution_gripper_command_duration_is_valid(actuation, *duration) ||
      !ros_gripper_output_mode_is_valid(actuation, output) ||
      (timeout != nullptr &&
       (timeout_seconds == nullptr ||
        !ros_gripper_delivery_result_timeout_is_valid(actuation,
                                                     *timeout_seconds)))) {
    fail(component.component_id + " has invalid authored gripper target semantics");
  }
  // The normal graph schema and strict applicability decoder verify the
  // complete row before any retained product or runtime admission is published.
  return actuation == "pneumatic" ? "manyforge.gripper_logical_cancellation"
                                 : "manyforge.gripper_position_cancellation";
}

// CP9: measured joints remain required. A Real pneumatic gripper has no
// gripper-feedback stream; the other lanes retain their existing requirement.
void require_component_stream_rows(const ActivationDeploymentComponent &component,
                                   const CanonicalValue &configuration,
                                   const ActivationComponentProfile &profile) {
  namespace contract = ros2_control_execution_runtime_contract;
  const auto rows = [&](std::string_view name) {
    const auto *selected = configuration.find_object_member(name);
    return selected == nullptr ? Array{} : array(*selected, name);
  };
  std::set<std::string> expected{std::string(contract::kMeasuredJointsStreamId)};
  for (const auto &row : rows("gripper_target_configurations")) {
    if (text(member(row, "controllable_component_id", "gripper target"),
             "gripper component") == component.component_id) {
      if (!real_controller_profile(profile) ||
          text(member(row, "actuation_kind", "gripper target"), "gripper actuation") !=
              "pneumatic")
        expected.insert(std::string(contract::kGripperFeedbackStreamId));
    }
  }
  std::set<std::string> declared;
  for (const auto &row : rows("component_streams")) {
    if (text(member(row, "controllable_component_id", "component stream"),
             "stream component") != component.component_id ||
        !declared.insert(text(member(row, "stream_id", "component stream"),
                              "stream id")).second) {
      fail(component.component_id + " has a foreign or repeated component stream");
    }
  }
  if (declared != expected) {
    fail(component.component_id +
         " component_streams must declare exactly measured_joints" +
         (expected.size() == 2U ? " and gripper_feedback" : ""));
  }
}

void validate_mapped_pneumatic_presets(
    const ActivationDeploymentComponent &component,
    const ActivationComponentProfile &profile,
    const ActivationPreparedModel &model) {
  const auto *configuration = component.configuration.find_object_member("runtime_configuration");
  const auto *grippers = configuration ? configuration->find_object_member("gripper_target_configurations") : nullptr;
  if (!grippers) return;
  const auto &rows = array(*grippers, "gripper_target_configurations");
  if (std::none_of(rows.begin(), rows.end(), [&](const auto &row) {
        return row.find_object_member("pneumatic_io") &&
               text(member(row, "controllable_component_id", "gripper target"), "gripper component") == component.component_id &&
               text(member(row, "command_target_id", "gripper target"), "gripper target") == "gripper";
      })) return;
  const auto joints = source_joints(profile, "fingers");
  const auto &srdf = model.verified_asset.source_ir().srdf();
  if (joints.size() != 1U || !srdf) return;  // Existing model validation owns missing/ambiguous sources.
  const auto &semantics = member(profile.model_family, "semantics", "model family");
  const auto &groups = array(member(semantics, "model_group_role_bindings", "model semantics"),
                             "model_group_role_bindings");
  const auto group = std::find_if(groups.begin(), groups.end(), [](const auto &row) {
    return text(member(row, "model_group_role_id", "model group"), "model group role") == "gripper";
  });
  if (group == groups.end()) return;
  const auto &group_name = text(member(*group, "source_group_name", "model group"), "source group");
  const auto value_for = [&](std::string_view name) -> std::optional<double> {
    const auto state = std::find_if(srdf->group_states.begin(), srdf->group_states.end(),
        [&](const auto &row) { return row.name == name && row.group_name == group_name; });
    if (state == srdf->group_states.end()) return std::nullopt;
    const auto joint = std::find_if(state->joints.begin(), state->joints.end(),
        [&](const auto &row) { return row.joint_name == joints.front(); });
    return joint == state->joints.end() ? std::nullopt : std::optional<double>(joint->value);
  };
  for (const auto &row : rows) {
    if (!row.find_object_member("pneumatic_io") ||
        text(member(row, "controllable_component_id", "gripper target"), "gripper component") != component.component_id ||
        text(member(row, "command_target_id", "gripper target"), "gripper target") != "gripper")
      continue;
    const auto open = value_for(text(member(row, "open_state_ref", "gripper target"), "open preset"));
    const auto closed = value_for(text(member(row, "closed_state_ref", "gripper target"), "closed preset"));
    if (open && closed && *open == *closed)
      fail("pneumatic_io_presets_coincident: " + component.component_id +
           " Open and Close resolve to the same gripper coordinate");
  }
}

[[nodiscard]] ComponentProjection project_component(
    const ActivationDeploymentComponent &component,
    const ActivationComponentProfile &profile,
    const ActivationPreparedModel &model, CatalogSelection &catalog,
    std::string_view runtime_unit_id,
    std::span<const QualifiedProviderManifest> provider_manifests) {
  if (!component.root_pose_in_cell || !component.model_source)
    fail("physical component projection requires its actual captured model and "
         "pose");
  const bool ros2_control =
      profile.runtime_kind == ActivationRuntimeKind::kRos2Control;
  if (ros2_control) {
    const auto *selected = profile.runtime_restoration_profile.find_object_member("ros2_control_runtime");
    const auto *configuration = component.configuration.find_object_member("runtime_configuration");
    const auto *grippers = configuration ? configuration->find_object_member("gripper_target_configurations") : nullptr;
    if (selected && controller_profile_selector(*selected) == std::string_view("isaac_profile") && grippers) {
      for (const auto &row : array(*grippers, "gripper_target_configurations"))
        if (text(member(row, "controllable_component_id", "gripper target"), "gripper component") ==
                component.component_id && row.find_object_member("pneumatic_io"))
          fail("pneumatic_io_unsupported_on_isaac: Isaac uses its existing gripper transport");
    }
    validate_mapped_pneumatic_presets(component, profile, model);
  }
  const bool has_gripper = has_gripper_target(component, profile);
  const std::string prefix = "ros2_control";
  // Authored portable identifiers permit punctuation that cannot name the
  // existing Isaac controller namespace. Refuse it before runtime launch.
  if (ros2_control &&
      component.component_id.find_first_of(".-") != std::string::npos) {
    fail(component.component_id + " must be one ROS name token");
  }
  const auto &component_ref = catalog.require(
      core::CatalogRecordKind::kComponentType, component_type_id(profile, has_gripper));
  const auto &runtime_ref =
      catalog.require(core::CatalogRecordKind::kRuntimeProfile,
                      "manyforge.ros2_control_execution_runtime");
  static_cast<void>(runtime_ref);
  static_cast<void>(catalog.require(core::CatalogRecordKind::kModelAsset,
                                    profile.model_asset_id));
  const auto &configuration = member(component.configuration,
                                     "runtime_configuration", "configuration");
  std::optional<CanonicalValue> controller_profile_row;
  if (ros2_control) {
    const auto &profile_configuration =
        member(profile.runtime_restoration_profile, "ros2_control_runtime",
               "component profile");
    const auto selector = controller_profile_selector(profile_configuration);
    if (!selector.has_value()) {
      fail(component.component_id + " (profile " +
           profile.component_profile_id +
           ") has no ROS controller profile in the catalog");
    }
    if (controller_profile_selector(configuration) != selector ||
        member(configuration, *selector, "runtime configuration") !=
            member(profile_configuration, *selector, "component profile")) {
      fail(component.component_id + " " + std::string(*selector) +
           " does not match its ROS component profile");
    }
    Object row{
        {"controllable_component_id", CanonicalValue(component.component_id)},
        {"frame_prefix", CanonicalValue(component.frame_prefix)},
        {std::string(*selector),
         member(configuration, *selector, "runtime configuration")},
    };
    // CP8 step 1: a Real row names its address through a site-settings
    // reference (never an address); the other lanes carry none.
    const auto *device_address_ref =
        configuration.find_object_member("device_address_ref");
    if ((*selector == "real_profile") != (device_address_ref != nullptr)) {
      fail(component.component_id +
           (device_address_ref == nullptr
                ? " real_profile requires a device_address_ref"
                : " device_address_ref belongs to a real_profile only"));
    }
    if (device_address_ref != nullptr) {
      row.emplace_back("device_address_ref", *device_address_ref);
    }
    // D11: the component's tool payload rides its controller profile row;
    // only a Real component's vendor adapter applies it (at Enable).
    if (const auto *payload = configuration.find_object_member("tool_payload");
        payload != nullptr) {
      row.emplace_back("tool_payload", *payload);
    }
    controller_profile_row = CanonicalValue(std::move(row));
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
    if (configuration.find_object_member("default_fault_stop_profiles") !=
        nullptr) {
      fail("runtime_configuration.default_fault_stop_profiles must be "
           "authored as configuration.default_fault_stop_profile");
    }
    require_component_stream_rows(component, configuration, profile);
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
  if (controller_profile_row.has_value()) {
    // Q68: the unit carries each component's own controller profile.
    for (const std::string_view field :
         {"isaac_profile", "mock_profile", "real_profile", "device_address_ref",
          "tool_payload"})
      runtime_configuration = object_without(runtime_configuration, field);
    runtime_configuration = object_with(
        runtime_configuration, "controller_profiles",
        CanonicalValue(Array{std::move(*controller_profile_row)}));
  }
  if (const auto *fault_profile =
          component.configuration.find_object_member(
              "default_fault_stop_profile");
      ros2_control && fault_profile != nullptr) {
    runtime_configuration = object_with(
        runtime_configuration, "default_fault_stop_profiles",
        CanonicalValue(Array{fault_stop_profile_row(component, *fault_profile)}));
  }
  if (component.configuration.find_object_member(
          "native_lifecycle_management") != nullptr) {
    fail("native_lifecycle_management is obsolete; native component lifecycle "
         "is selected by the qualified provider product");
  }

  // The Real arm is measured; its pneumatic gripper has only commanded
  // expected-position display until a physical feedback source is configured.
  const bool real = real_controller_profile(profile);
  const std::string arm_provider_id = ros2_control_provider_id(profile, "arm");
  const auto &arm_provider_ref = catalog.require(
      core::CatalogRecordKind::kProviderManifest, arm_provider_id);
  const auto arm_provider_manifest = std::find_if(
      provider_manifests.begin(), provider_manifests.end(),
      [&](const auto &manifest) {
        return manifest.value().provider_manifest_id.value() == arm_provider_id &&
               manifest.value().provider_manifest_revision.value() ==
                   revision(arm_provider_ref);
      });
  if (arm_provider_manifest == provider_manifests.end())
    fail(arm_provider_id + " manifest has no exact qualified source");
  // The manifest's whole offer closure is part of the selected compiler
  // catalog even when this arm-only model does not instantiate a gripper.
  for (const auto &offer : arm_provider_manifest->value().offered_capabilities) {
    const auto selected = selector(catalog, offer.capability_id.value());
    if (member(selected, "capability_definition_revision", "capability")
            .as_string() != offer.capability_definition_revision.value())
      fail(arm_provider_id +
           " manifest capability differs from its exact qualified definition");
  }
  // The selected ROS provider manifest still offers the gripper capabilities;
  // its offer closure needs their exact definitions without a gripper target.
  if (!has_gripper)
    for (const auto capability : kGripperCapabilities)
      static_cast<void>(selector(catalog, capability));
  Array domains;
  Array bindings;
  for (const std::string_view target : {std::string_view("arm"),
                                        std::string_view("gripper")}) {
    // DX783: arm-only models expose no synthetic gripper target or resource.
    if (target == "gripper" && !has_gripper)
      continue;
    const bool measured = real && target == "arm";
    const std::string_view authority_suffix = measured ? ".measured_state"
        : real ? ".commanded_state" : ".commanded_sim_state";
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
                        : gripper_stop_policy_id(component, runtime_configuration));
    const std::string provider_id = ros2_control_provider_id(profile, target);
    const auto &provider = catalog.require(
        core::CatalogRecordKind::kProviderManifest, provider_id);
    domains.emplace_back(Object{
        {"control_domain_id",
         CanonicalValue(domain_id(component.component_id, target))},
        {"control_domain_revision", CanonicalValue(std::string(kZeroDigest))},
        {"runtime_unit_ids",
         CanonicalValue(Array{CanonicalValue(std::string(runtime_unit_id))})},
        {"control_coordinator_endpoint_role",
         CanonicalValue("ros2_control_execution_status")},
        {"controller_topology_id", CanonicalValue(topology_id)},
        {"topology_revision", CanonicalValue(revision(topology))},
        {"containment_granularity", CanonicalValue("resource")},
    });
    const std::span<const std::string_view> capabilities =
        target == "arm"
            ? arm_capabilities()
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
              // domains (a Real gripper's is its own, see above).
              {"provider_instance_id",
               CanonicalValue(ros2_control_provider_instance_id(
                   component, profile, target))},
              {"provider_runtime_unit_id",
               CanonicalValue(std::string(runtime_unit_id))},
              {"provider_manifest_id", CanonicalValue(provider_id)},
              {"provider_manifest_revision",
               CanonicalValue(revision(provider))},
              {"state_authority_id",
               CanonicalValue(component.component_id + "." +
                              std::string(target) +
                              std::string(authority_suffix))},
              {"state_authority_class",
               CanonicalValue(measured ? "measured" : "commanded")},
              {"stop_evidence_policy_id",
               CanonicalValue(core::catalog_record_id_value(stop))},
              {"stop_evidence_policy_revision", CanonicalValue(revision(stop))},
              {"containment_granularity", CanonicalValue("resource")},
          });
      if (target == "arm" &&
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
          {"component_type_id", CanonicalValue(component_type_id(profile, has_gripper))},
          {"component_type_revision", CanonicalValue(revision(component_ref))},
          {"model_role_bindings",
           CanonicalValue(Array{CanonicalValue(Object{
               {"model_role_id", CanonicalValue("arm_model")},
               {"model_instance_id",
                CanonicalValue(component.component_id + ".model")},
           })})},
          {"runtime_unit_ids",
           CanonicalValue(Array{CanonicalValue(std::string(runtime_unit_id))})},
          {"root_pose_in_cell", *component.root_pose_in_cell},
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
      {"provider_contract_role_id",
       CanonicalValue(hard ? "software_hard_stop" : "stop_contract")},
      {"contract_schema_id",
       CanonicalValue(hard ? "manyforge.value_schema.provider_contract.empty.v1"
                           : "manyforge.value_schema.provider_contract.motion_"
                             "controlled_stop.v1")},
      {"contract_schema_revision",
       CanonicalValue(hard ? "sha256:"
                             "fb466dd341076cbdb6fc97558ad28bc46032d25daaf4d4bb9"
                             "d548bf642b83270"
                           : "sha256:"
                             "65e6f4462981225916834490d4e3cb4d42b70611a2287e1b7"
                             "6fdb1cbcabcb113")},
  });
}

/// C++ port of the maintained generator's `assistant_projection`, without
/// external resources or site overlays: every model alias is served by one
/// AI model component of the Deployment (`external_resource_alias` is its
/// component id, `model_id` its served model). An alias selects
/// `mapping[alias]`, else the only AI model component. Typed refusals:
/// `assistant_ai_model_component_missing` (no AI model component),
/// `assistant_model_component_unresolved` (unknown mapping key or component,
/// or several components without a mapping entry).
[[nodiscard]] CanonicalValue assistant_member(
    const ActivationAssistantProfile &source,
    const std::map<std::string, std::string, std::less<>> &mapping,
    const std::map<std::string, std::string, std::less<>> &ai_models) {
  const auto &profile = source.profile;
  const auto &endpoints =
      array(member(profile, "model_endpoints", "assistant profile"),
            "assistant profile.model_endpoints");
  for (const auto &[alias, unused] : mapping) {
    static_cast<void>(unused);
    if (std::none_of(endpoints.begin(), endpoints.end(),
                     [&](const auto &endpoint) {
                       return text(member(endpoint, "model_endpoint_alias",
                                          "model endpoint"),
                                   "model_endpoint_alias") == alias;
                     }))
      fail("assistant_model_component_unresolved: " + alias +
           " is not a model alias of the assistant profile");
  }
  Object model_aliases;
  for (const auto &endpoint : endpoints) {
    const auto &alias =
        text(member(endpoint, "model_endpoint_alias", "model endpoint"),
             "model_endpoint_alias");
    const auto mapped = mapping.find(alias);
    if (mapped == mapping.end() && ai_models.empty())
      fail("assistant_ai_model_component_missing: the assistant profile needs "
           "an AI model component");
    if (mapped == mapping.end() && ai_models.size() != 1U)
      fail("assistant_model_component_unresolved: " + alias +
           " needs an assistant_model_components entry");
    const auto selected = mapped == mapping.end()
                              ? ai_models.begin()
                              : ai_models.find(mapped->second);
    if (selected == ai_models.end())
      fail("assistant_model_component_unresolved: " + mapped->second +
           " is not an AI model component");
    model_aliases.emplace_back(
        alias, CanonicalValue(Object{
                   {"external_resource_alias", CanonicalValue(selected->first)},
                   {"model_id", CanonicalValue(selected->second)},
                   {"request_contract",
                    member(endpoint, "request_contract", "model endpoint")},
               }));
  }
  Object provider_aliases;
  const auto &provider = member(profile, "provider_alias", "assistant profile");
  if (!provider.is_null()) {
    provider_aliases.emplace_back(
        text(member(provider, "provider_alias_id", "provider alias"),
             "provider_alias_id"),
        CanonicalValue(Object{
            {"provider_kind",
             member(provider, "provider_kind", "provider alias")},
            {"bridge_external_resource_alias",
             member(provider, "bridge_external_resource_alias",
                    "provider alias")},
        }));
  }
  Object modes;
  for (const auto &[mode_id, mode] : object(source.modes, "assistant modes")) {
    if (provider.is_null() ||
        std::none_of(model_aliases.begin(), model_aliases.end(),
                     [](const auto &row) { return row.first == "assistant_model"; }))
      fail("assistant profile has incomplete mode aliases");
    auto row = object(mode, "assistant mode");
    row.emplace_back("provider_alias", provider_aliases.front().first);
    row.emplace_back("model_endpoint_alias", CanonicalValue("assistant_model"));
    modes.emplace_back(mode_id, CanonicalValue(std::move(row)));
  }
  return CanonicalValue(Object{
      {"audit_policy", member(profile, "audit_policy", "assistant profile")},
      {"prompt_templates",
       member(profile, "prompt_templates", "assistant profile")},
      {"model_endpoint_aliases", CanonicalValue(std::move(model_aliases))},
      {"provider_aliases", CanonicalValue(std::move(provider_aliases))},
      {"modes", CanonicalValue(std::move(modes))},
  });
}

} // namespace

CanonicalValue compile_activation_deployment(
    const ActivationDeploymentIntent &intent,
    const core::CellId &runtime_cell_id,
    const std::vector<ActivationComponentProfile> &component_profiles,
    const std::vector<ActivationPreparedModel> &prepared_models,
    const std::vector<core::CatalogRecordRefDTO> &catalog_records,
    const std::vector<QualifiedRuntimeProfile> &runtime_profiles,
    const std::vector<ActivationPreparedIoType> &io_types,
    std::span<const QualifiedProviderManifest> provider_manifests,
    const std::optional<ActivationAssistantProfile> &assistant_profile) {
  const auto physical_count = static_cast<std::size_t>(std::count_if(
      component_profiles.begin(), component_profiles.end(),
      [](const auto &profile) {
        return !is_model_free_runtime_kind(profile.runtime_kind);
      }));
  if (intent.components.size() != component_profiles.size() ||
      physical_count != prepared_models.size() ||
      io_types.size() + physical_count != intent.components.size()) {
    fail("component/profile/model joins differ in size");
  }
  // D8: authored declarations must reach an installed runtime policy owner.
  // This producer still emits empty interrupt tables below; refuse required
  // declarations here instead of silently dropping them from the activation.
  for (const std::string_view field : {"interrupt_sources", "interrupt_policies"}) {
    if (const auto *declarations = intent.policies.find_object_member(field);
        declarations != nullptr && !array(*declarations, field).empty()) {
      fail(std::string(field) +
           ": authored interrupt declarations are not installed by this compiler");
    }
  }
  if (!array(intent.attachments, "attachments").empty()) {
    fail("component attachments are not yet supported by the authored compiler");
  }
  CatalogSelection catalog(catalog_records);
  const bool ros2_control = true;
  const auto prepared_model =
      [&](std::size_t component_index) -> const ActivationPreparedModel & {
    const auto index = static_cast<std::size_t>(std::count_if(
        component_profiles.begin(),
        component_profiles.begin() + component_index, [](const auto &profile) {
          return !is_model_free_runtime_kind(profile.runtime_kind);
        }));
    return prepared_models.at(index);
  };
  // Q89: empty membership keeps the same ROS cell host and optional world.
  // Q68: a ROS composition admits any number of robots and components, each
  // of which must select a ROS controller profile from the catalog. Mixing
  // with native components stays refused until the native executor goes.
  const auto has_ros_controller_profile = [](const auto &profile) {
    if (is_model_free_runtime_kind(profile.runtime_kind))
      return true;
    if (profile.runtime_kind != ActivationRuntimeKind::kRos2Control) {
      return false;
    }
    const auto *runtime =
        profile.runtime_restoration_profile.find_object_member(
            "ros2_control_runtime");
    return runtime != nullptr &&
           controller_profile_selector(*runtime).has_value();
  };
  const auto lacking_ros_controller_profile = [&] {
    return std::find_if_not(component_profiles.begin(),
                            component_profiles.end(),
                            has_ros_controller_profile);
  };
  if (std::any_of(component_profiles.begin(), component_profiles.end(),
                  [&](const auto &profile) {
                    return profile.runtime_kind !=
                               ActivationRuntimeKind::kRos2Control &&
                           !is_model_free_runtime_kind(profile.runtime_kind);
                  })) {
    const auto lacking = lacking_ros_controller_profile();
    fail("mixed native and ROS runtime compositions are not yet supported: "
         "component " +
         lacking->component_id + " (profile " + lacking->component_profile_id +
         ") has no ROS controller profile in the catalog");
  }
  if (!ros2_control) {
    const auto lacking = lacking_ros_controller_profile();
    fail("component " + lacking->component_id + " (profile " +
         lacking->component_profile_id +
         ") has no ROS controller profile in the catalog");
  }
  if (ros2_control) {
    if (const auto lacking = lacking_ros_controller_profile();
        lacking != component_profiles.end()) {
      fail("component " + lacking->component_id + " (profile " +
           lacking->component_profile_id +
           ") has no ROS controller profile in the catalog");
    }
  }
  const std::string runtime_profile_id =
      std::string(ros2_control_execution_runtime_contract::kRuntimeProfileId);
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
    if (!object(intent.policies, "policies").empty()) {
      fail("empty Deployment has no component policies");
    }
    if (intent.assistant_profile_id.has_value()) {
      fail("assistant_ai_model_component_missing: an empty Deployment has no "
           "AI model component");
    }
    auto authored = ros2_control_execution_runtime_contract::empty_host_deployment(
        runtime_cell_id, catalog_value(runtime_ref), intent.deployment_artifact_id,
        intent.name).as_object();
    authored.emplace_back("compatibility", CanonicalValue(Object{
        {"lane", CanonicalValue("manyforge-only")},
        {"provides", CanonicalValue(Array{})},
        {"tags", CanonicalValue(Array{})},
    }));
    return compute_deployment_artifact_revision(
        parse_strict_json_model_yaml(canonical_json(CanonicalValue(std::move(authored)))), selected_profiles).normalized_authored_root();
  }
  const std::string runtime_id =
      std::string(ros2_control_execution_runtime_contract::kCellHostRuntimeUnitId);
  if (ros2_control) {
    std::map<std::string, std::string> frame_owners;
    bool shared_world_seen = false;
    for (std::size_t index = 0; index < intent.components.size(); ++index) {
      const auto &component = intent.components[index];
      if (is_model_free_runtime_kind(component_profiles[index].runtime_kind))
        continue;
      const auto &urdf =
          prepared_model(index).verified_asset.source_ir().urdf();
      for (const auto &link : urdf.links) {
        // Only the unprefixed root world is the shared global parent. A
        // prefixed link named world is still a component-owned published frame.
        const bool shared_world = component.frame_prefix.empty() && link.name == "world" &&
            std::none_of(urdf.joints.begin(), urdf.joints.end(), [&](const auto &joint) {
              return joint.child_link_name == link.name;
            });
        const auto frame = component.frame_prefix + link.name;
        if (shared_world && shared_world_seen) continue;
        const auto [owner, inserted] = frame_owners.emplace(frame, component.component_id);
        if (!inserted) {
          fail("published TF frame collision: " + frame + " between " +
               owner->second + " and " + component.component_id);
        }
        if (shared_world) shared_world_seen = true;
      }
    }
  }
  std::vector<ComponentProjection> projections;
  projections.reserve(intent.components.size());
  for (std::size_t index = 0; index < intent.components.size(); ++index) {
    const auto &component = intent.components[index];
    if (is_model_free_runtime_kind(component_profiles[index].runtime_kind)) {
      const auto found = std::find_if(
          io_types.begin(), io_types.end(), [&](const auto &value) {
            return value.component_id == component.component_id;
          });
      if (found == io_types.end() ||
          std::count_if(io_types.begin(), io_types.end(),
                        [&](const auto &value) {
                          return value.component_id == component.component_id;
                        }) != 1)
        fail("model-free component has no exact qualified derived type");
      const auto &type_ref =
          catalog.require(core::CatalogRecordKind::kComponentType,
                          found->type.value().component_type_id.value());
      if (revision(type_ref) !=
          found->type.value().component_type_revision.value())
        fail("model-free derived component type differs from actual catalog "
             "referent");
      auto configuration =
          ros2_control_execution_runtime_contract::empty_host_configuration();
      // The normalized wrapper holds exactly the unit rows of its kind.
      for (const auto &[name, rows] :
           object(found->normalized_configuration, "model-free configuration"))
        configuration = object_with(configuration, name, rows);
      projections.push_back(
          {std::move(configuration),
           CanonicalValue(nullptr),
           CanonicalValue(Object{
               {"controllable_component_id",
                CanonicalValue(component.component_id)},
               {"component_type_id",
                CanonicalValue(found->type.value().component_type_id.value())},
               {"component_type_revision",
                CanonicalValue(
                    found->type.value().component_type_revision.value())},
               {"model_role_bindings", CanonicalValue(Array{})},
               {"runtime_unit_ids",
                CanonicalValue(Array{CanonicalValue(runtime_id)})}}),
           {},
           {}});
    } else {
      projections.push_back(
          project_component(component, component_profiles[index],
                            prepared_model(index), catalog, runtime_id,
                            provider_manifests));
    }
  }

  Array controller_profiles;
  for (const auto &projection : projections) {
    const auto *rows = projection.runtime_configuration.find_object_member(
        "controller_profiles");
    if (rows == nullptr)
      continue;
    controller_profiles.insert(controller_profiles.end(),
                               rows->as_array().begin(),
                               rows->as_array().end());
  }
  require_no_exclusive_ros_controller_topic_collision(controller_profiles);
  // One robot is driven by one component: two Real components naming the
  // same site device reference would command the same controller twice.
  std::set<std::string> device_address_refs;
  for (const auto &row : controller_profiles) {
    const auto *ref = row.find_object_member("device_address_ref");
    if (ref != nullptr &&
        !device_address_refs.insert(text(*ref, "device_address_ref")).second) {
      fail("real_device_address_duplicate: more than one Real component names "
           "device_address_ref " + text(*ref, "device_address_ref"));
    }
  }

  const auto physical = std::find_if(
      component_profiles.begin(), component_profiles.end(),
      [](const auto &profile) {
        return !is_model_free_runtime_kind(profile.runtime_kind);
      });
  const auto base_index =
      physical == component_profiles.end()
          ? 0U
          : static_cast<std::size_t>(physical - component_profiles.begin());
  CanonicalValue runtime_configuration =
      projections[base_index].runtime_configuration;
  if (projections.size() > 1U) {
    for (const std::string_view name : {"planner_rng_seed",
                                        "execution_tick_hz",
                                        "collision_monitor_period_ms"}) {
      const auto &expected = member(runtime_configuration, name, "runtime configuration");
      for (std::size_t index = 0; index != projections.size(); ++index) {
        if (is_model_free_runtime_kind(component_profiles[index].runtime_kind))
          continue;
        const auto &projection = projections[index];
        if (member(projection.runtime_configuration, name,
                   "runtime configuration") != expected) {
          fail("grouped native components disagree on " + std::string(name));
        }
      }
    }
    using MergedFields = std::array<std::string_view, 10U>;
    // Q68: one ROS runtime unit hosts every component and each component
    // keeps its own controller profile, fault Stop profile, authored
    // gripper target rows and stream defaults; the ROS runtime schema has no
    // native direct-control field.
    const MergedFields merged_fields = MergedFields{
        "initial_joint_positions_rad", "motion_target_limits",
        "default_fault_stop_profiles", "gripper_target_configurations",
        "controller_profiles",         "io_signals",
        "io_provider_devices",         "pose_sources",
        "ai_models",                   "component_streams"};
    for (const std::string_view name : merged_fields) {
      if (name.empty()) continue;
      Array combined;
      bool present = false;
      for (const auto &projection : projections) {
        const auto *selected = projection.runtime_configuration.find_object_member(name);
        if (selected == nullptr) continue;
        present = true;
        const auto &values = array(*selected, name);
        combined.insert(combined.end(), values.begin(), values.end());
      }
      if (ros2_control && !present) continue;
      if (name != "initial_joint_positions_rad") {
        std::sort(combined.begin(), combined.end(), [](const auto &left, const auto &right) {
          const auto key = [](const auto &row) {
            const auto *component = row.find_object_member("controllable_component_id");
            const auto *target = row.find_object_member("command_target_id");
            const auto *joint = row.find_object_member("source_joint_name");
            const auto *resource = row.find_object_member("resource_role_id");
            const auto *stream = row.find_object_member("stream_id");
            return std::tuple(component == nullptr ? ""
                                                   : component->as_string(),
                              target == nullptr ? "" : target->as_string(),
                              joint == nullptr ? "" : joint->as_string(),
                              resource == nullptr ? "" : resource->as_string(),
                              stream == nullptr ? "" : stream->as_string());
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
    if (!projection.model_instance.is_null())
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
  const auto ready = prepare_cell_graph_identity(intrinsic, selected_profiles);
  const auto computed = compute_graph_revision(ready);
  const auto &computed_graph = computed.normalized_root();

  Array desired;
  Array providers;
  Array stop_stages;
  for (std::size_t index = 0; index < intent.components.size(); ++index) {
    const auto &component = intent.components[index];
    const auto &profile = component_profiles[index];
    if (is_model_free_runtime_kind(profile.runtime_kind))
      continue;
    const auto &model_urdf =
        prepared_model(index).verified_asset.source_ir().urdf();
    for (const std::string_view target : {std::string_view("arm"),
                                          std::string_view("gripper")}) {
      if (target == "gripper" && !has_gripper_target(component, profile))
        continue;
      const std::string topology_id =
          std::string("manyforge.") +
          ("ros2_control_") + std::string(target) +
          "_direct";
      const auto &topology = catalog.require(
          core::CatalogRecordKind::kControllerTopology, topology_id);
      const std::string domain = domain_id(component.component_id, target);
      const std::span<const std::string_view> capabilities =
          target == "arm"
              ? arm_capabilities()
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
          core::CatalogRecordKind::kComponentType,
          component_type_id(profile, has_gripper_target(component, profile)));
      const std::string planning_provider_id =
          ros2_control_provider_id(profile, target);
      const auto &planning =
          catalog.require(core::CatalogRecordKind::kProviderManifest,
                          planning_provider_id);
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
                 CanonicalValue(ros2_control_provider_instance_id(
                     component, profile, target))},
                {"provider_manifest_id", CanonicalValue(planning_provider_id)},
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
                               CanonicalValue parameters,
                               std::optional<CanonicalValue> contract = std::nullopt) {
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
              {"provider_contract_ref", contract.has_value() ? *contract
                                                             : provider_contract_ref(hard)},
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
            // The one Stop deceleration limit per joint (the runtime derives
            // both the operator and the fault Stop from it) must not be lower
            // than the acceleration ordinary planning may use for that joint.
            if (const auto *deceleration = std::get_if<double>(
                    &member(limit, "max_deceleration_rad_s2", "limit").storage());
                deceleration != nullptr) {
              if (const auto error = stop_deceleration_coherence_error(
                      component.component_id + "." + std::string(target) +
                          ".controlled_stop",
                      joints[index], *deceleration,
                      planning_joint_acceleration_limit_rad_s2(model_urdf,
                                                               joints[index]));
                  error.has_value()) {
                fail(*error);
              }
            }
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
          Object controlled_parameters{
              {"stop_mode", member(controlled, "stop_mode", "controlled stop")},
              {"jerk_limits_enabled", member(controlled, "jerk_limits_enabled", "controlled stop")},
              {"deceleration_time_s", member(controlled, "deceleration_time_s", "controlled stop")},
              {"per_resource_limits", CanonicalValue(std::move(resource_limits))},
              {"allow_natural_completion_on_short_path", member(controlled, "allow_natural_completion_on_short_path", "controlled stop")},
          };
          const auto *legacy_fallback =
              controlled.find_object_member("recoverable_substitution_mode");
          const auto *fallback_modes =
              controlled.find_object_member("fallback_stop_modes");
          if (legacy_fallback && fallback_modes)
            fail("controlled Stop has conflicting fallback fields");
          if (legacy_fallback) {
            if (text(member(controlled, "stop_mode", "controlled stop"),
                     "stop mode") != "soft_on_path" ||
                text(*legacy_fallback, "legacy fallback") != "spring_back_hold")
              fail("invalid legacy Stop fallback");
            controlled_parameters.emplace_back(
                "fallback_stop_modes",
                CanonicalValue(Array{CanonicalValue("spring_back_hold")}));
          }
          if (fallback_modes) {
            const auto &modes =
                array(*fallback_modes, "controlled Stop fallbacks");
            if (modes.size() > 2U)
              fail("too many Stop fallbacks");
            const auto &primary =
                text(member(controlled, "stop_mode", "controlled stop"),
                     "stop mode");
            int previous = primary == "soft_on_path"       ? 0
                           : primary == "spring_back_hold" ? 1
                           : primary == "spring_back_fast" ? 2
                                                           : 3;
            for (const auto &item : modes) {
              const auto &mode = text(item, "Stop fallback");
              const int current = mode == "spring_back_hold"   ? 1
                                  : mode == "spring_back_fast" ? 2
                                                               : -1;
              if (current <= previous)
                fail("Stop fallbacks must be ordered below primary, excluding "
                     "hard");
              previous = current;
            }
            if (!modes.empty())
              controlled_parameters.emplace_back("fallback_stop_modes",
                                                 *fallback_modes);
          }
          // Q28/Q51 (`main`): the per-profile maximum Stop time, when authored.
          if (const auto *max_stop = controlled.find_object_member("max_stop_time_s");
              max_stop != nullptr) {
            const auto *seconds = std::get_if<double>(&max_stop->storage());
            if (seconds == nullptr || !std::isfinite(*seconds) || *seconds <= 0.0) {
              fail("controlled Stop max_stop_time_s must be a finite positive number");
            }
            controlled_parameters.emplace_back("max_stop_time_s", CanonicalValue(*seconds));
          }
          stage("motion_controlled_stop", false, CanonicalValue(std::move(controlled_parameters)));
        }
        if (target == "arm") {
          stage("software_hard", true, CanonicalValue(Object{}));
        } else {
          const CanonicalValue logical_contract(Object{
              {"provider_contract_role_id", CanonicalValue("gripper_logical_cancel")},
              {"contract_schema_id", CanonicalValue("manyforge.value_schema.provider_contract.empty.v1")},
              {"contract_schema_revision", CanonicalValue(
                  "sha256:fb466dd341076cbdb6fc97558ad28bc46032d25daaf4d4bb9d548bf642b83270")},
          });
          stage("gripper_logical_cancel", false, CanonicalValue(Object{}), logical_contract);
          if (text(member(binding, "stop_evidence_policy_id", "binding"),
                   "gripper Stop policy") == "manyforge.gripper_position_cancellation") {
            stage("software_hard", true, CanonicalValue(Object{}));
          }
        }
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
  // Model-free provider families: the I/O signal provider, the CP9 pose
  // source and the AI model. Each selects its exact maintained manifest once.
  struct ModelFreeProvider final {
    std::string_view manifest_id;
    std::string_view instance_prefix;
    std::string_view binding_prefix;
    std::optional<core::CatalogRecordRefDTO> manifest;
    std::span<const core::ProviderCapabilityOfferDTO> offers;
  };
  ModelFreeProvider io_provider{core::kIoProviderLifecycleManifestId,
                                "provider.io.", "io.binding.", {}, {}};
  ModelFreeProvider pose_provider{core::kPoseSourceLifecycleManifestId,
                                  "provider.pose_source.",
                                  "pose_source.binding.", {}, {}};
  ModelFreeProvider ai_model_provider{core::kAiModelLifecycleManifestId,
                                      "provider.ai_model.",
                                      "ai_model.binding.", {}, {}};
  const auto provider_for =
      [&](std::string_view component_id) -> ModelFreeProvider & {
    const auto profile = std::find_if(
        component_profiles.begin(), component_profiles.end(),
        [&](const auto &row) { return row.component_id == component_id; });
    if (profile != component_profiles.end() &&
        profile->runtime_kind == ActivationRuntimeKind::kPoseSource)
      return pose_provider;
    if (profile != component_profiles.end() &&
        profile->runtime_kind == ActivationRuntimeKind::kAiModel)
      return ai_model_provider;
    return io_provider;
  };
  const auto select_manifest = [&](ModelFreeProvider &provider) {
    const std::string manifest_id(provider.manifest_id);
    provider.manifest = catalog.require(
        core::CatalogRecordKind::kProviderManifest, manifest_id);
    const auto actual = std::find_if(
        provider_manifests.begin(), provider_manifests.end(),
        [&](const auto &manifest) {
          return manifest.value().provider_manifest_id.value() == manifest_id &&
                 manifest.value().provider_manifest_revision.value() ==
                     revision(*provider.manifest);
        });
    if (actual == provider_manifests.end())
      fail(manifest_id + " manifest has no exact qualified source");
    provider.offers = actual->value().offered_capabilities;
    // The manifest's complete offer closure remains selected even when this
    // device only instantiates inputs. A capability offer does not install an
    // output.
    for (const auto &offer : actual->value().offered_capabilities) {
      const auto selected = selector(catalog, offer.capability_id.value());
      if (member(selected, "capability_definition_revision", "capability")
              .as_string() != offer.capability_definition_revision.value())
        fail(manifest_id +
             " manifest capability differs from its exact qualified "
             "definition");
    }
  };
  for (const auto &prepared : io_types) {
    auto &provider = provider_for(prepared.component_id);
    if (!provider.manifest)
      select_manifest(provider);
  }
  if (io_provider.manifest) {
    const bool output =
        std::any_of(io_types.begin(), io_types.end(), [](const auto &prepared) {
          return std::any_of(prepared.type.value().command_targets.begin(),
                             prepared.type.value().command_targets.end(),
                             [](const auto &target) {
                               return std::any_of(
                                   target.capabilities.begin(),
                                   target.capabilities.end(),
                                   [](const auto &capability) {
                                     return capability.capability_id.value() ==
                                            "io.gpio.set_output";
                                   });
                             });
        });
    if (output)
      static_cast<void>(catalog.require(core::CatalogRecordKind::kCommandSchema,
                                        core::kIoSignalWriteCommandSchemaId));
  }
  const auto io_provider_row = [&](const ModelFreeProvider &provider,
                                   std::string_view capability,
                                   std::string_view instance,
                                   std::string_view scope,
                                   CanonicalValue target) {
    const auto selected = selector(catalog, capability);
    const auto offer = std::find_if(
        provider.offers.begin(), provider.offers.end(),
        [&](const auto &candidate) {
          return candidate.capability_id.value() == capability &&
                 candidate.capability_definition_revision.value() ==
                     member(selected, "capability_definition_revision",
                            "capability").as_string();
        });
    if (offer == provider.offers.end())
      fail("model-free binding has no exact qualified capability offer");
    Array endpoint_bindings;
    for (const auto &role : offer->required_endpoint_role_ids) {
      if (role.value() != "io_signal_service")
        fail("I/O offer endpoint role has no qualified runtime projection");
      endpoint_bindings.emplace_back(Object{
          {"endpoint_role_id", CanonicalValue(role.value())},
          {"source_kind", CanonicalValue("runtime_unit")},
          {"source_id", CanonicalValue(runtime_id)},
          {"source_endpoint_role_id",
           CanonicalValue("ros2_control_execution_status")}});
    }
    const auto identity = content_identity::framed_sha256(
        "io-capability-provider-binding", "manyforge.io_provider_binding.v1",
        CanonicalValue(Object{
            {"provider_instance_id", CanonicalValue(std::string(instance))},
            {"capability_id", CanonicalValue(std::string(capability))},
            {"target", target}}));
    Object row{
        {"capability_provider_binding_id",
         CanonicalValue(std::string(provider.binding_prefix) +
                        identity.substr(7U))},
        {"capability_provider_binding_revision",
         CanonicalValue(std::string(kZeroDigest))},
        {"scope", CanonicalValue(std::string(scope))},
        {"target", std::move(target)},
        {"provider_instance_id", CanonicalValue(std::string(instance))},
        {"provider_manifest_id",
         CanonicalValue(std::string(provider.manifest_id))},
        {"provider_manifest_revision",
         CanonicalValue(revision(*provider.manifest))},
        {"endpoint_bindings", CanonicalValue(std::move(endpoint_bindings))}};
    for (const auto &field : selected.as_object())
      row.push_back(field);
    return CanonicalValue(std::move(row));
  };
  for (const auto &prepared : io_types) {
    const auto &provider = provider_for(prepared.component_id);
    const auto identity = content_identity::framed_sha256(
        "io-provider-instance", "manyforge.io_provider_instance.v1",
        CanonicalValue(Object{{"runtime_unit_id", CanonicalValue(runtime_id)},
                              {"controllable_component_id",
                               CanonicalValue(prepared.component_id)}}));
    const auto instance =
        std::string(provider.instance_prefix) + identity.substr(7U);
    for (const auto &target : prepared.type.value().command_targets)
      for (const auto &capability : target.capabilities)
        providers.push_back(io_provider_row(
            provider, capability.capability_id.value(), instance,
            "target_binding",
            CanonicalValue(Object{
                {"controllable_component_id",
                 CanonicalValue(prepared.component_id)},
                {"component_type_revision",
                 CanonicalValue(
                     prepared.type.value().component_type_revision.value())},
                {"command_target_id",
                 CanonicalValue(target.command_target_id.value())}})));
    for (const auto capability :
         {"component.lifecycle.observe", "component.lifecycle.prepare",
          "component.lifecycle.enable", "component.lifecycle.disable",
          "component.lifecycle.recover"})
      static_cast<void>(selector(catalog, capability));
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
  for (std::size_t index = 0; index < component_profiles.size(); ++index)
    if (!is_model_free_runtime_kind(component_profiles[index].runtime_kind) &&
        has_gripper_target(intent.components[index], component_profiles[index])) {
      static_cast<void>(catalog.require(core::CatalogRecordKind::kCommandSchema,
                                        "gripper.command_position"));
      break;
    }
  std::sort(desired.begin(), desired.end(), by("control_domain_id"));
  std::sort(providers.begin(), providers.end(), by("capability_provider_binding_id"));
  std::sort(stop_stages.begin(), stop_stages.end(), [](const auto &left, const auto &right) {
    return std::tuple(text(member(left, "control_binding_id", "stage"), "id"),
                      text(member(left, "stop_escalation_stage_id", "stage"), "stage")) <
           std::tuple(text(member(right, "control_binding_id", "stage"), "id"),
                      text(member(right, "stop_escalation_stage_id", "stage"), "stage"));
  });

  const bool has_policies =
      physical_count != 0U || !intent.policies.as_object().empty();
  Object policies;
  if (has_policies) {
    policies = {
        {"retained_policy_records", CanonicalValue(Array{})},
        {"stop_stage_parameters", CanonicalValue(std::move(stop_stages))},
        {"commissioned_stop_interfaces", CanonicalValue(Array{})},
        {"interrupt_sources", CanonicalValue(Array{})},
        {"interrupt_policies", CanonicalValue(Array{})},
        {"interrupt_config",
         member(intent.policies, "interrupt_config", "policies")},
    };
    if (const auto *rules =
            intent.policies.find_object_member("interrupt_rules");
        rules && !array(*rules, "interrupt_rules").empty())
      policies.emplace_back("interrupt_rules", *rules);
    if (const auto *sources =
            intent.policies.find_object_member("scene_pose_sources"))
      policies.emplace_back("scene_pose_sources", *sources);
    // The shared Deployment decoder below owns row and graph validation.
    if (const auto *streams =
            intent.policies.find_object_member("component_streams"))
      policies.emplace_back("component_streams", *streams);
    if (const auto *budget = intent.policies.find_object_member(
            "motion_takeover_preparation_budget_s")) {
      // The shared Deployment decoder below owns positive-finite validation.
      policies.emplace_back("motion_takeover_preparation_budget_s", *budget);
    }
    if (const auto *budget = intent.policies.find_object_member(
            "motion_takeover_planning_budget_s")) {
      policies.emplace_back("motion_takeover_planning_budget_s", *budget);
    }
  }
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
  };
  if (has_policies)
    root.emplace_back("policies", CanonicalValue(std::move(policies)));
  if (intent.description.has_value())
    root.emplace_back("description", CanonicalValue(*intent.description));
  if (intent.compatibility.has_value())
    root.emplace_back("compatibility", *intent.compatibility);
  if (intent.assistant_profile_id.has_value()) {
    if (!assistant_profile.has_value())
      fail("assistant profile has no repository-owned source");
    // Component id -> served model of every AI model component.
    std::map<std::string, std::string, std::less<>> ai_models;
    for (std::size_t index = 0; index != component_profiles.size(); ++index) {
      if (component_profiles[index].runtime_kind !=
          ActivationRuntimeKind::kAiModel)
        continue;
      const auto found = std::find_if(
          io_types.begin(), io_types.end(), [&](const auto &value) {
            return value.component_id == component_profiles[index].component_id;
          });
      if (found == io_types.end())
        fail("AI model component has no prepared configuration");
      ai_models.emplace(
          found->component_id,
          text(member(array(member(found->normalized_configuration,
                                   "ai_models", "AI model configuration"),
                            "ai_models")
                          .front(),
                      "model", "AI model row"),
               "model"));
    }
    root.emplace_back("assistant",
                      assistant_member(*assistant_profile,
                                       intent.assistant_model_components,
                                       ai_models));
  }
  CanonicalValue authored(std::move(root));
  const auto preliminary = compute_deployment_artifact_revision(
      parse_strict_json_model_yaml(canonical_json(authored)),
      selected_profiles);
  const auto derived = DeriveIoProviderDomains(
      preliminary.graph().graph(),
      preliminary.applicability_inputs().value().capability_provider_bindings,
      selected_profiles, provider_manifests);
  if (!derived.ok())
    fail("model-free provider derivation: " +
         derived.error().details.at("reason"));
  if (!derived.value().provider_domains.empty()) {
    auto graph_value = member(authored, "graph", "Deployment");
    auto domains = member(graph_value, "control_domains", "graph").as_array();
    auto provider_rows =
        member(authored, "capability_provider_bindings", "Deployment")
            .as_array();
    for (const auto &domain : derived.value().provider_domains) {
      domains.push_back(IoProviderDomainRecord(domain));
      for (const auto capability :
           {"component.lifecycle.observe", "component.lifecycle.prepare",
            "component.lifecycle.enable", "component.lifecycle.disable",
            "component.lifecycle.recover"})
        provider_rows.push_back(io_provider_row(
            provider_for(
                domain.provider()->controllable_component_id.value()),
            capability, domain.provider()->provider_instance_id.value(),
            "control_domain",
            CanonicalValue(
                Object{{"control_domain_id",
                        CanonicalValue(domain.control_domain_id.value())},
                       {"control_domain_revision",
                        CanonicalValue(domain.control_domain_revision.value())},
                       {"domain_kind", CanonicalValue("provider")}})));
    }
    graph_value = object_with(graph_value, "control_domains",
                              CanonicalValue(std::move(domains)));
    authored = object_with(object_with(authored, "graph", graph_value),
                           "capability_provider_bindings",
                           CanonicalValue(std::move(provider_rows)));
  }
  return compute_deployment_artifact_revision(
             parse_strict_json_model_yaml(canonical_json(authored)),
             selected_profiles)
      .normalized_authored_root();
}

} // namespace manyforge::deployment::detail
