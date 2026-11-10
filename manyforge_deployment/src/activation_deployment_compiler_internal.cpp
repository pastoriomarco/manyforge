#include "activation_deployment_compiler_internal.hpp"
#include "component_static_frames_internal.hpp"
#include "execution_host_unit_internal.hpp"
#include "ros2_control_execution_runtime_unit_internal.hpp"
#include "stop_deceleration_coherence_internal.hpp"
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_deployment/io_provider_domain.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <map>
#include <numbers>
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
#include <manyforge_core/types/typed_error.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/cell_composition.hpp>
#include <manyforge_deployment/cell_graph_codec.hpp>
#include <manyforge_deployment/component_spec.hpp>
#include <manyforge_deployment/deployment_artifact_codec.hpp>
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>

namespace manyforge::deployment::detail {
namespace {

using Array = CanonicalValue::Array;
using Object = CanonicalValue::Object;

constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";

constexpr std::array<std::string_view, 15U> kRosArmCapabilities{{
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
constexpr std::array<std::string_view, 2U> kExecutionCoreCapabilities{{
    "execution.interrupt.contact_policy",
    "execution.interrupt.resource_scoped",
}};
constexpr std::array<std::string_view, 5U> kSimulatorCapabilities{{
    "simulator.control.pause",
    "simulator.control.play",
    "simulator.control.reset",
    "simulator.control.step",
    "simulator.entity_state.set",
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

// A component's authored lifecycle time budget (`configuration.
// lifecycle_command_timeout_s`) becomes its own keyed row of the unit's
// `lifecycle_command_timeouts`. I/O devices keep theirs in
// `io_provider_devices`.
[[nodiscard]] CanonicalValue with_lifecycle_budget(
    const CanonicalValue &runtime_configuration,
    const ActivationDeploymentComponent &component) {
  const auto *budget =
      component.configuration.find_object_member("lifecycle_command_timeout_s");
  if (budget == nullptr) return runtime_configuration;
  return object_with(
      runtime_configuration, "lifecycle_command_timeouts",
      CanonicalValue(Array{CanonicalValue(Object{
          {"controllable_component_id", CanonicalValue(component.component_id)},
          {"lifecycle_command_timeout_s", *budget},
      })}));
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

// The execution core is always present in the cell host. Its interrupt
// capabilities are cell services and require no external runtime endpoint.
// An empty cell is infrastructure-only and carries no capabilities.
[[nodiscard]] Array execution_core_provider_rows(CatalogSelection &catalog,
                                                 const core::CellId &runtime_cell_id) {
  const auto &execution_core = catalog.require(
      core::CatalogRecordKind::kProviderManifest,
      "manyforge.provider.execution_core");
  Array rows;
  for (const auto capability : kExecutionCoreCapabilities) {
    std::string normalized(capability);
    std::replace(normalized.begin(), normalized.end(), '.', '-');
    Object row{
        {"capability_provider_binding_id", CanonicalValue(
            runtime_cell_id.value() + "." + normalized + ".provider")},
        {"capability_provider_binding_revision", CanonicalValue(std::string(kZeroDigest))},
    };
    const auto selected_capability = selector(catalog, capability);
    for (const auto &selected : object(selected_capability, "selector"))
      row.push_back(selected);
    row.insert(row.end(), {
        {"scope", CanonicalValue("cell_global")},
        {"target", CanonicalValue(Object{{"cell_id", CanonicalValue(runtime_cell_id.value())}})},
        {"provider_instance_id", CanonicalValue(runtime_cell_id.value() + ".execution_core")},
        {"provider_manifest_id", CanonicalValue("manyforge.provider.execution_core")},
        {"provider_manifest_revision", CanonicalValue(revision(execution_core))},
        {"endpoint_bindings", CanonicalValue(Array{})},
    });
    rows.emplace_back(std::move(row));
  }
  return rows;
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

// A tool's model has no arm joints: its one target is `gripper`, and it is
// usually mounted on a robot's `flange` through an `attachments` row.
[[nodiscard]] bool is_tool(const ActivationComponentProfile &profile) {
  return source_joints(profile, "arm_joints").empty();
}

[[nodiscard]] std::string
component_type_id(const ActivationComponentProfile &profile,
                  const bool has_gripper) {
  if (is_model_free_runtime_kind(profile.runtime_kind))
    throw std::logic_error(
        "model-free type must come from its actual qualified structural "
        "artifact");
  // A Real combined arm+gripper model without an authored gripper remains
  // arm-only. An authored Real pneumatic gripper uses the same model-backed
  // target as the Mock component. Split arm models have no fingers.
  if (profile.lane == std::optional<std::string>("real") && !has_gripper &&
      !source_joints(profile, "fingers").empty())
    return profile.model_asset_id + ".arm_only.component";
  return profile.model_asset_id + ".component";
}

// An arm's lane row declares whether its adapter's controller accepts
// runtime speed scaling (`runtime_scale: supported | unsupported`); the
// arm's motion binding carries a runtime-scale qualification only when it
// does.
[[nodiscard]] bool runtime_scale_supported(const ActivationComponentProfile &profile) {
  const auto &declared =
      text(member(*profile.lane_row, "runtime_scale", profile.component_profile_id + " lane row"),
           "runtime_scale");
  if (declared != "supported" && declared != "unsupported")
    fail(profile.component_profile_id + " lane row runtime_scale must be supported or "
         "unsupported");
  return declared == "supported";
}

// A pneumatic tool profile runs on no lane (its row declares no
// `lanes`). Its lane lives in the I/O components its `pneumatic_io`
// references, so one tool row serves every lane.
[[nodiscard]] bool lane_free_tool(const ActivationComponentProfile &profile) {
  return profile.runtime_kind == ActivationRuntimeKind::kRos2Control &&
         is_tool(profile) && !profile.lane.has_value();
}

// A real robot reports measured state; Mock and Isaac stay
// commanded.
[[nodiscard]] bool real_controller_profile(
    const ActivationComponentProfile &profile) {
  return profile.runtime_kind == ActivationRuntimeKind::kRos2Control &&
         profile.lane == std::optional<std::string>("real");
}

// Every lane uses the one shared ros2_control provider: the vendor adapter
// the execution host reads from the model's `lanes.real.adapter` row
// does the vendor sequence, so no provider names a vendor.
constexpr std::string_view kRos2ControlProviderId =
    "manyforge.provider.ros2_control";

[[nodiscard]] std::string ros2_control_provider_instance_id(
    const ActivationDeploymentComponent &component) {
  return component.component_id + ".ros2_control";
}

// A Real robot and a lane-free tool run no gripper controller of their own.
[[nodiscard]] bool without_gripper_controller(
    const ActivationComponentProfile &profile) {
  return real_controller_profile(profile) || lane_free_tool(profile);
}

// Do not infer a gripper from passive Real model geometry. A Real or lane-free
// gripper is present only when the component explicitly authors its pneumatic
// target.
[[nodiscard]] bool has_gripper_target(const ActivationDeploymentComponent &component,
                                      const ActivationComponentProfile &profile) {
  if (source_joints(profile, "fingers").empty()) return false;
  if (!without_gripper_controller(profile)) return true;
  const bool real = real_controller_profile(profile);
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
        fail(real ? component.component_id + " Real gripper must be pneumatic"
                  : "tool_lane_required: " + component.component_id +
                        " only a pneumatic tool may omit its controller lane");
      if (text(member(row, "output_mode", "gripper target"), "gripper output") ==
              "commanded" && row.find_object_member("pneumatic_io") == nullptr)
        fail(real ? "real_pneumatic_io_not_wired: " + component.component_id +
                        " commanded pneumatic I/O is not yet wired"
                  : "tool_pneumatic_io_required: " + component.component_id +
                        " a commanded lane-free pneumatic tool needs pneumatic_io");
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
motion_limits(const ActivationDeploymentComponent &component) {
  Array result;
  for (const auto &[target, settings] : object(component.target_settings,
                                                "target_settings")) {
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
        {"ordinary_motion_jerk", std::move(jerk)},
    };
    result.emplace_back(std::move(limit));
  }
  return CanonicalValue(std::move(result));
}

// Carry the component's authored `default_fault_stop_profile` (main's
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

/// An arm's authored `controlled_stop.per_joint_limits`, one row per
/// arm source joint, keyed by that joint (never by row position), each Stop
/// deceleration no lower than the acceleration ordinary planning may use for
/// the joint. Its two outputs are the arm's Stop stage rows and its
/// controller profile's `max_deceleration_on_cancel_rad_s2`, so the JTC's
/// cancel bound and the Kernel's Stop plan are the same values.
[[nodiscard]] std::map<std::string, const CanonicalValue *, std::less<>>
controlled_stop_limits(const ActivationDeploymentComponent &component,
                       const ActivationComponentProfile &profile,
                       const ActivationPreparedModel &model) {
  const auto &settings = member(component.target_settings, "arm", "target settings");
  const auto &controlled = member(settings, "controlled_stop", "arm settings");
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
  const auto &urdf = model.verified_asset.source_ir().urdf();
  for (const auto &joint : joints) {
    const auto found = limit_by_joint.find(joint);
    if (found == limit_by_joint.end()) fail("controlled Stop limits miss joint " + joint);
    // The one Stop deceleration limit per joint (the runtime derives both the
    // operator and the fault Stop from it) must not be lower than the
    // acceleration ordinary planning may use for that joint.
    const auto *deceleration = std::get_if<double>(
        &member(*found->second, "max_deceleration_rad_s2", "limit").storage());
    if (deceleration == nullptr) fail("controlled Stop max_deceleration_rad_s2 for joint " +
                                      joint + " must be a number");
    if (const auto error = stop_deceleration_coherence_error(
            component.component_id + ".arm.controlled_stop", joint, *deceleration,
            planning_joint_acceleration_limit_rad_s2(urdf, joint));
        error.has_value()) {
      fail(*error);
    }
  }
  return limit_by_joint;
}

/// The controller profile's `max_deceleration_on_cancel_rad_s2` rows, sorted
/// by source joint.
[[nodiscard]] CanonicalValue max_deceleration_on_cancel_rows(
    const std::map<std::string, const CanonicalValue *, std::less<>> &limits) {
  Array rows;
  for (const auto &[joint, limit] : limits)
    rows.emplace_back(Object{
        {"max_deceleration_rad_s2", member(*limit, "max_deceleration_rad_s2", "limit")},
        {"source_joint_name", CanonicalValue(joint)},
    });
  return CanonicalValue(std::move(rows));
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
  const auto *pneumatic_io = selected->find_object_member("pneumatic_io");
  if (fields.size() != 13U + static_cast<std::size_t>(pneumatic_io != nullptr) +
                           static_cast<std::size_t>(
                               selected->find_object_member("grasp_confirmation") != nullptr) +
                           static_cast<std::size_t>(
                               selected->find_object_member("stop_finish_tolerance") != nullptr) ||
      duration == nullptr ||
      !execution_gripper_command_duration_is_valid(actuation, *duration) ||
      !ros_gripper_output_mode_is_valid(actuation, output)) {
    fail(component.component_id + " has invalid authored gripper target semantics");
  }
  // The normal graph schema and strict applicability decoder verify the
  // complete row before any retained product or runtime admission is published.
  return actuation == "pneumatic" ? "manyforge.gripper_logical_cancellation"
                                 : "manyforge.gripper_position_cancellation";
}

// Measured joints remain required. A pneumatic gripper without its own
// controller (Real robot, lane-free tool) has no gripper-feedback stream; the
// other lanes retain their existing requirement.
void require_component_stream_rows(const ActivationDeploymentComponent &component,
                                   const CanonicalValue &configuration,
                                   const ActivationComponentProfile &profile) {
  namespace contract = ros2_control_execution_runtime_contract;
  const auto rows = [&](std::string_view name) {
    const auto *selected = configuration.find_object_member(name);
    return selected == nullptr ? Array{} : array(*selected, name);
  };
  // A lane-free tool runs no controller manager: it publishes no joint
  // states and no gripper feedback, so it declares no stream.
  std::set<std::string> expected;
  if (!lane_free_tool(profile))
    expected.insert(std::string(contract::kMeasuredJointsStreamId));
  for (const auto &row : rows("gripper_target_configurations")) {
    if (text(member(row, "controllable_component_id", "gripper target"),
             "gripper component") == component.component_id) {
      if (!without_gripper_controller(profile) ||
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
         (expected.empty() ? " is a lane-free tool: component_streams must "
                             "declare no stream"
                           : " component_streams must declare exactly measured_joints" +
                                 std::string(expected.size() == 2U
                                                 ? " and gripper_feedback"
                                                 : "")));
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
      // Typed: the GUI explains it by its catalog code; the text is unchanged.
      throw core::TypedError("pneumatic_io_presets_coincident",
          "Activation Deployment compiler: pneumatic_io_presets_coincident: " +
              component.component_id + " Open and Close resolve to the same gripper coordinate");
  }
}

// An intent pose `{position_m, orientation: {rpy_deg} | {xyzw}}` as the graph
// pose `{position_m, orientation_xyzw}`. rpy_deg is fixed-axis X-Y-Z (URDF
// roll, pitch, yaw); q = Rz(yaw) Ry(pitch) Rx(roll).
[[nodiscard]] CanonicalValue graph_pose(const CanonicalValue &pose) {
  const auto &orientation = member(pose, "orientation", "pose");
  const auto numbers = [](const CanonicalValue &value) {
    std::vector<double> result;
    for (const auto &item : value.as_array())
      result.push_back(std::get<double>(item.storage()));
    return result;
  };
  std::vector<double> xyzw;
  if (const auto *rpy = orientation.find_object_member("rpy_deg")) {
    const auto angles = numbers(*rpy);
    const double half = std::numbers::pi_v<double> / 360.0;
    const double cr = std::cos(angles[0] * half), sr = std::sin(angles[0] * half);
    const double cp = std::cos(angles[1] * half), sp = std::sin(angles[1] * half);
    const double cy = std::cos(angles[2] * half), sy = std::sin(angles[2] * half);
    xyzw = {sr * cp * cy - cr * sp * sy, cr * sp * cy + sr * cp * sy,
            cr * cp * sy - sr * sp * cy, cr * cp * cy + sr * sp * sy};
  } else {
    xyzw = numbers(member(orientation, "xyzw", "pose.orientation"));
  }
  Array orientation_xyzw;
  for (const double value : xyzw)
    orientation_xyzw.emplace_back(value == 0.0 ? 0.0 : value);  // no -0
  return CanonicalValue(Object{
      {"position_m", member(pose, "position_m", "pose")},
      {"orientation_xyzw", CanonicalValue(std::move(orientation_xyzw))},
  });
}

// A component row's custom TCPs as graph rows: each parent resolves to a
// link of the component's model, through a frame role of its asset (a
// descriptor TCP, link-backed or offset) or else a URDF link. A custom TCP
// never hangs off another custom TCP, and its id names no existing frame.
[[nodiscard]] std::optional<CanonicalValue>
custom_tcp_rows(const ActivationDeploymentComponent &component,
                const SourceVerifiedModelAsset &asset) {
  if (component.custom_tcps.empty()) return std::nullopt;
  const auto &bindings = asset.value().frame_role_bindings;
  const auto &links = asset.source_ir().urdf().links;
  const auto role = [&](std::string_view id) {
    return std::find_if(bindings.begin(), bindings.end(), [&](const auto &row) {
      return row.frame_role_id.value() == id;
    });
  };
  const auto link = [&](std::string_view name) {
    return std::any_of(links.begin(), links.end(),
                       [&](const auto &row) { return row.name == name; });
  };
  Array rows;
  for (const auto &custom : component.custom_tcps) {
    const std::string label =
        component.component_id + " custom TCP '" + custom.tcp_id + "'";
    if (role(custom.tcp_id) != bindings.end() || link(custom.tcp_id))
      fail("custom_tcp_name_collision: " + label +
           " names an existing frame of the component");
    const bool chained = std::any_of(
        component.custom_tcps.begin(), component.custom_tcps.end(),
        [&](const auto &other) { return other.tcp_id == custom.parent_frame; });
    std::string parent_link;
    if (const auto found = role(custom.parent_frame); found != bindings.end())
      parent_link = core::frame_role_link_name(*found);
    else if (!chained && link(custom.parent_frame))
      parent_link = custom.parent_frame;
    else
      fail("custom_tcp_parent_unknown: " + label + " has parent '" +
           custom.parent_frame + "', which is " +
           (chained ? "another custom TCP" : "no frame of the component"));
    rows.emplace_back(Object{
        {"frame_role_id", CanonicalValue(custom.tcp_id)},
        {"parent_link_name", CanonicalValue(std::move(parent_link))},
        {"pose_in_parent_link", graph_pose(custom.pose)},
    });
  }
  return CanonicalValue(std::move(rows));
}

// The model's source URDF (its URDF entrypoint's bytes): the launched
// description starts from it.
[[nodiscard]] const std::string &source_urdf(const ActivationPreparedModel &model) {
  for (const auto &row : model.verified_asset.retrieval_plan().artifacts()) {
    if (row.artifact_kind != ModelSourceArtifactKind::kUrdfXmlEntrypoint) continue;
    for (const auto &artifact : model.source_artifacts)
      if (artifact.logical_uri == row.logical_uri) return artifact.bytes;
  }
  fail(model.verified_asset.value().model_asset_id.value() + " has no source URDF");
}

[[nodiscard]] ComponentProjection project_component(
    const ActivationDeploymentComponent &component,
    const ActivationComponentProfile &profile,
    const ActivationPreparedModel &model, CatalogSelection &catalog,
    std::string_view runtime_unit_id,
    std::span<const QualifiedProviderManifest> provider_manifests,
    std::span<const QualifiedComponentType> component_types) {
  // A mounted child has no root pose: the attachment places it.
  if (!component.model_source)
    fail("physical component projection requires its actual captured model");
  const bool ros2_control =
      profile.runtime_kind == ActivationRuntimeKind::kRos2Control;
  const bool tool = is_tool(profile);
  const bool lane_free = lane_free_tool(profile);
  if (ros2_control) {
    const auto *configuration = component.configuration.find_object_member("runtime_configuration");
    const auto *grippers = configuration ? configuration->find_object_member("gripper_target_configurations") : nullptr;
    if (profile.lane == std::optional<std::string>("isaac") && grippers) {
      for (const auto &row : array(*grippers, "gripper_target_configurations"))
        if (text(member(row, "controllable_component_id", "gripper target"), "gripper component") ==
                component.component_id && row.find_object_member("pneumatic_io"))
          throw core::TypedError("pneumatic_io_unsupported_on_isaac",
              "Activation Deployment compiler: pneumatic_io_unsupported_on_isaac: Isaac uses "
              "its existing gripper transport");
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
  const auto component_type = std::find_if(
      component_types.begin(), component_types.end(), [&](const auto &type) {
        return type.value().component_type_id.value() ==
                   core::catalog_record_id_value(component_ref) &&
               type.value().component_type_revision.value() == revision(component_ref);
      });
  if (component_type == component_types.end())
    fail(core::catalog_record_id_value(component_ref) +
         " has no qualified component type");
  const auto &runtime_ref =
      catalog.require(core::CatalogRecordKind::kRuntimeProfile,
                      "manyforge.ros2_control_execution_runtime");
  static_cast<void>(runtime_ref);
  static_cast<void>(catalog.require(core::CatalogRecordKind::kModelAsset,
                                    profile.model_asset_id));
  const auto &configuration = member(component.configuration,
                                     "runtime_configuration", "configuration");
  if (configuration.find_object_member("tool_payload") != nullptr) {
    fail(component.component_id +
         " runtime_configuration.tool_payload moved: a tool's payload comes "
         "from its model (tool descriptor payload) and attached objects' "
         "mass_kg");
  }
  std::optional<CanonicalValue> controller_profile_row;
  if (ros2_control && !lane_free) {
    if (!profile.lane.has_value() || !profile.lane_row.has_value()) {
      fail(component.component_id + " (model " +
           profile.component_profile_id + ") names no lane");
    }
    // The model's `lanes.<lane>` row travels
    // verbatim as canonical JSON, so the execution host builds the
    // component's adapter without reading the model folder.
    Object row{
        {"controllable_component_id", CanonicalValue(component.component_id)},
        {"frame_prefix", CanonicalValue(component.frame_prefix)},
        {"lane", CanonicalValue(*profile.lane)},
        {"lane_row", CanonicalValue(canonical_json(*profile.lane_row))},
        {"model_family_id", CanonicalValue(profile.model_family_id)},
    };
    // A Real row names its address through a site-settings
    // reference (never an address); the other lanes carry none. A machine's
    // arm member takes its owner's and has none of its own.
    if (profile.owner_component_id.has_value()) {
      if (configuration.find_object_member("device_address_ref") != nullptr)
        fail(component.component_id + " is a member of " +
             *profile.owner_component_id +
             ": its device_address_ref belongs to the owner");
      row.emplace_back("owner_component_id",
                       CanonicalValue(*profile.owner_component_id));
      row.emplace_back("member_role", CanonicalValue(*profile.member_role));
    }
    const auto *device_address_ref =
        profile.owner_component_id.has_value()
            ? (profile.owner_device_address_ref
                   ? &*profile.owner_device_address_ref
                   : nullptr)
            : configuration.find_object_member("device_address_ref");
    const bool real = *profile.lane == "real";
    if (real != (device_address_ref != nullptr)) {
      fail(component.component_id +
           (device_address_ref == nullptr
                ? " lane real requires a device_address_ref"
                : " device_address_ref belongs to lane real only"));
    }
    if (device_address_ref != nullptr) {
      row.emplace_back("device_address_ref", *device_address_ref);
    }
    // The component's soft Stop lead-in anchor (absent: commanded).
    if (const auto *anchor = configuration.find_object_member("stop_lead_in_anchor")) {
      row.emplace_back("stop_lead_in_anchor", *anchor);
    }
    // The component's controller TCP offset sync (absent: on).
    if (const auto *sync = configuration.find_object_member("controller_tcp_offset_sync")) {
      row.emplace_back("controller_tcp_offset_sync", *sync);
    }
    // What the host launches the component with, so it reads no model
    // or planning product for it: an arm's source joints in controller order
    // (its `arm_joints`), a tool's gripper joint, and its description (the
    // model URDF; compile_activation_deployment adds the cell's static
    // frames once the cell is composed).
    if (tool) {
      row.emplace_back("gripper_joint_name",
                       CanonicalValue(source_joints(profile, "fingers").front()));
    } else {
      Array joints;
      for (auto &joint : source_joints(profile, "arm_joints"))
        joints.emplace_back(std::move(joint));
      row.emplace_back("joint_names", CanonicalValue(std::move(joints)));
    }
    row.emplace_back("description", CanonicalValue(source_urdf(model)));
    // An arm launches its JTC with its operator Stop decelerations.
    if (!tool)
      row.emplace_back("max_deceleration_on_cancel_rad_s2",
                       max_deceleration_on_cancel_rows(
                           controlled_stop_limits(component, profile, model)));
    controller_profile_row = CanonicalValue(std::move(row));
  }
  if (!tool) {
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
  }
  const auto limits = motion_limits(component);
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
    if (configuration.find_object_member("lifecycle_command_timeouts") !=
        nullptr) {
      fail("runtime_configuration.lifecycle_command_timeouts must be "
           "authored as configuration.lifecycle_command_timeout_s");
    }
    require_component_stream_rows(component, configuration, profile);
    const auto &rows = array(limits, "motion limits");
    if (tool && !rows.empty())
      fail(component.component_id + " is a tool: it has no TCP-speed maximum");
    if (!tool && (rows.size() != 1U ||
                  text(member(rows.front(), "command_target_id", "motion limit"),
                       "target") != "arm")) {
      fail("authored ROS requires exactly one TCP-speed maximum for arm and no "
           "unknown target");
    }
  }
  CanonicalValue runtime_configuration =
      object_with(configuration, "motion_target_limits", limits);
  if (controller_profile_row.has_value()) {
    // The unit carries each component's own controller profile.
    for (const std::string_view field :
         {"device_address_ref", "stop_lead_in_anchor",
          "controller_tcp_offset_sync"})
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
  runtime_configuration = with_lifecycle_budget(runtime_configuration, component);
  if (component.configuration.find_object_member(
          "native_lifecycle_management") != nullptr) {
    fail("native_lifecycle_management is obsolete; native component lifecycle "
         "is selected by the qualified provider product");
  }

  // The Real arm is measured; its pneumatic gripper has only commanded
  // expected-position display until a physical feedback source is configured.
  // A lane-free tool's gripper is the same commanded display.
  const bool real = real_controller_profile(profile);
  const std::string arm_provider_id(kRos2ControlProviderId);
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
    // Arm-only models expose no synthetic gripper target or resource.
    // A tool has no arm target.
    if ((target == "gripper" && !has_gripper) || (target == "arm" && tool))
      continue;
    const bool measured = real && target == "arm";
    const std::string_view authority_suffix = measured ? ".measured_state"
        : real || lane_free ? ".commanded_state" : ".commanded_sim_state";
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
    const std::string provider_id(kRos2ControlProviderId);
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
               CanonicalValue(ros2_control_provider_instance_id(component))},
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
      if (target == "arm" && capability == "execution.trajectory.execute" &&
          runtime_scale_supported(profile)) {
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
  Object component_record{
      {"controllable_component_id", CanonicalValue(component.component_id)},
      {"component_type_id", CanonicalValue(component_type_id(profile, has_gripper))},
      {"component_type_revision", CanonicalValue(revision(component_ref))},
      {"model_role_bindings",
       CanonicalValue(Array{CanonicalValue(Object{
           {"model_role_id", CanonicalValue(tool ? "tool_model" : "arm_model")},
           {"model_instance_id",
            CanonicalValue(component.component_id + ".model")},
       })})},
      {"runtime_unit_ids",
       CanonicalValue(Array{CanonicalValue(std::string(runtime_unit_id))})},
  };
  if (component.root_pose_in_cell)
    component_record.emplace_back("root_pose_in_cell", *component.root_pose_in_cell);
  if (const auto tcp = compiled_default_tcp(component, component_type->value()))
    component_record.emplace_back(
        "default_tcp",
        CanonicalValue(Object{
            {"command_target_id", CanonicalValue(tcp->command_target_id.value())},
            {"frame_role_id", CanonicalValue(tcp->frame_role_id.value())},
        }));
  Object model_instance{
      {"model_instance_id", CanonicalValue(component.component_id + ".model")},
      {"model_asset_id", CanonicalValue(profile.model_asset_id)},
      {"model_revision",
       CanonicalValue(model.catalog_reference.record_revision.value())},
  };
  if (auto custom = custom_tcp_rows(component, model.verified_asset))
    model_instance.emplace_back("custom_tcps", std::move(*custom));
  return ComponentProjection{
      std::move(runtime_configuration),
      CanonicalValue(std::move(model_instance)),
      CanonicalValue(std::move(component_record)),
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

/// The chat alias: the model alias every chat mode uses, bound by the
/// assistant component (never by `assistant_model_components`).
constexpr std::string_view kChatModelAlias = "assistant_model";

/// The Deployment's `assistant` member, without external resources or site
/// overlays: every model alias is served by one AI model component of the
/// Deployment (`external_resource_alias` is its component id, `model_id` its
/// served model).
/// - The runtime aliases and prompt templates come from the Deployment-level
///   profile (`runtime`); an alias selects `mapping[alias]`, else the only AI
///   model component. Its chat alias is no runtime alias.
/// - The chat (the assistant component, `chat`) adds the chat alias, bound to
///   the component's `model_component_id`, and its profile's modes, each
///   served by that component.
/// The audit policy is the Deployment-level profile's, else the chat's. Null
/// when there is nothing to retain. Refusals:
/// `assistant_ai_model_component_missing` (no AI model component),
/// `assistant_model_component_unresolved` (unknown mapping key or component,
/// the chat alias mapped, or several components without a mapping entry).
[[nodiscard]] std::optional<CanonicalValue> assistant_member(
    const ActivationAssistantProfiles &profiles,
    const std::map<std::string, std::string, std::less<>> &mapping,
    const std::map<std::string, std::string, std::less<>> &ai_models) {
  Object model_aliases;
  const auto alias_of = [](const CanonicalValue &endpoint) -> const std::string & {
    return text(member(endpoint, "model_endpoint_alias", "model endpoint"),
                "model_endpoint_alias");
  };
  if (const auto found = mapping.find(kChatModelAlias); found != mapping.end())
    fail("assistant_model_component_unresolved: " + std::string(kChatModelAlias) +
         " is the chat alias; the assistant component binds it");
  if (profiles.runtime) {
    const auto &endpoints =
        array(member(profiles.runtime->profile, "model_endpoints", "assistant profile"),
              "assistant profile.model_endpoints");
    for (const auto &[alias, unused] : mapping) {
      static_cast<void>(unused);
      if (std::none_of(endpoints.begin(), endpoints.end(),
                       [&](const auto &endpoint) { return alias_of(endpoint) == alias; }))
        fail("assistant_model_component_unresolved: " + alias +
             " is not a model alias of the assistant profile");
    }
    for (const auto &endpoint : endpoints) {
      const auto &alias = alias_of(endpoint);
      if (alias == kChatModelAlias) continue;
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
  }
  Object modes;
  if (profiles.chat) {
    const auto &chat = *profiles.chat;
    const auto model = ai_models.find(chat.model_component_id);
    if (model == ai_models.end())
      fail("assistant_model_component_unresolved: " + chat.model_component_id +
           " is not an AI model component");
    const auto &endpoints =
        array(member(chat.profile.profile, "model_endpoints", "assistant profile"),
              "assistant profile.model_endpoints");
    const auto endpoint = std::find_if(endpoints.begin(), endpoints.end(),
                                       [&](const auto &row) {
                                         return alias_of(row) == kChatModelAlias;
                                       });
    if (endpoint == endpoints.end())
      fail("assistant profile has incomplete mode aliases");
    model_aliases.emplace_back(
        std::string(kChatModelAlias),
        CanonicalValue(Object{
            {"external_resource_alias", CanonicalValue(model->first)},
            {"model_id", CanonicalValue(model->second)},
            {"request_contract", member(*endpoint, "request_contract", "model endpoint")},
        }));
    for (const auto &[mode_id, mode] : object(chat.profile.modes, "assistant modes")) {
      auto row = object(mode, "assistant mode");
      row.emplace_back("assistant_component_id", CanonicalValue(chat.component_id));
      row.emplace_back("model_endpoint_alias", CanonicalValue(std::string(kChatModelAlias)));
      modes.emplace_back(mode_id, CanonicalValue(std::move(row)));
    }
  }
  const auto *source = profiles.runtime ? &profiles.runtime->profile
                       : profiles.chat  ? &profiles.chat->profile.profile
                                        : nullptr;
  const auto &templates = profiles.runtime
                              ? member(profiles.runtime->profile, "prompt_templates",
                                       "assistant profile")
                              : CanonicalValue(Object{});
  if (source == nullptr ||
      (model_aliases.empty() && modes.empty() && object(templates, "prompt_templates").empty()))
    return std::nullopt;
  return CanonicalValue(Object{
      {"audit_policy", member(*source, "audit_policy", "assistant profile")},
      {"prompt_templates", templates},
      {"model_endpoint_aliases", CanonicalValue(std::move(model_aliases))},
      {"modes", CanonicalValue(std::move(modes))},
  });
}

// Every controller profile's description (its model URDF so far) with
// the static frames of the composed cell appended: its own
// offset frames, the mounts on it and a process-less mounted child's fixed
// links. `graph` is the compiled cell graph; composition reads none of its
// runtime configuration, so the descriptions do not change it.
[[nodiscard]] CanonicalValue with_static_frames(
    const CanonicalValue &configuration, const GraphRevisionVerifiedCellGraph &graph,
    const std::vector<ActivationPreparedModel> &prepared_models,
    const std::vector<ActivationPreparedIoType> &io_types,
    std::span<const QualifiedComponentType> component_types) {
  std::vector<SourceVerifiedModelAsset> assets;
  std::vector<VerifiedRetainedModelSourceUrdf> sources;
  for (const auto &model : prepared_models) {
    const auto &asset = model.verified_asset.value();
    if (std::any_of(sources.begin(), sources.end(), [&](const auto &source) {
          return source.model_asset_id == asset.model_asset_id &&
                 source.model_revision == asset.model_revision;
        }))
      continue;
    std::vector<core::ModelFrameRoleBindingDTO> offset_frame_roles;
    for (const auto &binding : asset.frame_role_bindings)
      if (binding.pose_in_source_link.has_value()) offset_frame_roles.push_back(binding);
    assets.push_back(model.verified_asset);
    sources.push_back({asset.model_asset_id, asset.model_revision, source_urdf(model),
                       std::move(offset_frame_roles), asset.payload});
  }
  // The exact type closure of the graph's components (models and model-free).
  std::vector<QualifiedComponentType> types;
  for (const auto &component : graph.graph().controllable_components) {
    const auto same = [&](const QualifiedComponentType &type) {
      return type.value().component_type_id == component.component_type_id &&
             type.value().component_type_revision == component.component_type_revision;
    };
    if (std::any_of(types.begin(), types.end(), same)) continue;
    if (const auto found = std::find_if(component_types.begin(), component_types.end(), same);
        found != component_types.end()) {
      types.push_back(*found);
      continue;
    }
    const auto derived = std::find_if(io_types.begin(), io_types.end(),
                                      [&](const auto &row) { return same(row.type); });
    if (derived == io_types.end())
      fail(component.controllable_component_id.value() + " has no qualified component type");
    types.push_back(derived->type);
  }
  const auto composition = compose_cell_occurrences(graph, assets, types);
  Array profiles;
  for (const auto &row : array(member(configuration, "controller_profiles", "configuration"),
                               "controller_profiles")) {
    const core::ControllableComponentId id(
        text(member(row, "controllable_component_id", "controller profile"), "component"));
    const auto facts = component_description_facts(graph.graph(), graph.normalized_root(), id);
    profiles.push_back(object_with(
        row, "description",
        CanonicalValue(component_description(
            text(member(row, "description", "controller profile"), "description"),
            ComponentDescriptionInput{id, facts, composition.symbol_rewrites(),
                                      composition.fixed_connections(), sources}))));
  }
  return object_with(configuration, "controller_profiles", CanonicalValue(std::move(profiles)));
}

} // namespace

std::optional<core::ComponentDefaultTcpDTO> compiled_default_tcp(
    const ActivationDeploymentComponent &component,
    const core::ComponentTypeDTO &type) {
  std::vector<core::ComponentDefaultTcpDTO> tcps;
  std::vector<core::ComponentDefaultTcpDTO> declared;
  for (const auto &target : type.command_targets) {
    if (!target.default_tcp_frame_role_id.has_value()) continue;
    declared.push_back({target.command_target_id, *target.default_tcp_frame_role_id});
    for (const auto &frame : target.frame_roles)
      if (frame.tcp) tcps.push_back({target.command_target_id, frame.frame_role_id});
    for (const auto &custom : component.custom_tcps)
      tcps.push_back({target.command_target_id, core::FrameRoleId(custom.tcp_id)});
  }
  // Typed: the GUI explains both by their catalog code; the text is unchanged.
  if (component.default_tcp.has_value()) {
    if (std::find(tcps.begin(), tcps.end(), *component.default_tcp) == tcps.end())
      throw core::TypedError("component_default_tcp_unknown",
          "Activation Deployment compiler: component_default_tcp_unknown: " +
              component.component_id + " default_tcp {" +
              component.default_tcp->command_target_id.value() + ", " +
              component.default_tcp->frame_role_id.value() +
              "} is not a TCP of its targets");
    return component.default_tcp;
  }
  if (declared.size() == 1U) return declared.front();
  if (!tcps.empty())
    throw core::TypedError("component_default_tcp_required",
        "Activation Deployment compiler: component_default_tcp_required: " +
            component.component_id + " has TCPs on " +
            std::to_string(declared.size()) + " targets; choose its default_tcp");
  return std::nullopt;
}

CanonicalValue compile_activation_deployment(
    const ActivationDeploymentIntent &intent,
    const core::CellId &runtime_cell_id,
    const std::vector<ActivationComponentProfile> &component_profiles,
    const std::vector<ActivationPreparedModel> &prepared_models,
    const std::vector<core::CatalogRecordRefDTO> &catalog_records,
    const std::vector<QualifiedRuntimeProfile> &runtime_profiles,
    const std::vector<ActivationPreparedIoType> &io_types,
    std::span<const QualifiedProviderManifest> provider_manifests,
    const ActivationAssistantProfiles &assistant_profiles,
    std::span<const QualifiedComponentType> component_types) {
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
  // Each authored mount becomes one graph attachment, identified by its child
  // (one parent per child). The cell graph and structural validation own
  // references, interfaces, accepted tags and capacity.
  Array attachments;
  for (const auto &row : array(intent.attachments, "attachments")) {
    const auto endpoint = [&](std::string_view end) {
      const auto &value = member(row, end, "attachment");
      return CanonicalValue(Object{
          {"controllable_component_id", member(value, "component_id", "attachment")},
          {"attachment_interface_id", member(value, "interface", "attachment")},
      });
    };
    const auto *pose = row.find_object_member("pose");
    attachments.emplace_back(Object{
        {"component_attachment_id",
         member(member(row, "child", "attachment"), "component_id", "attachment")},
        {"component_attachment_revision", CanonicalValue(std::string(kZeroDigest))},
        {"parent", endpoint("parent")},
        {"child", endpoint("child")},
        {"child_interface_pose_in_parent_interface",
         pose != nullptr ? *pose
                         : CanonicalValue(Object{
                               {"position_m", CanonicalValue(Array{
                                   CanonicalValue(0.0), CanonicalValue(0.0),
                                   CanonicalValue(0.0)})},
                               {"orientation_xyzw", CanonicalValue(Array{
                                   CanonicalValue(0.0), CanonicalValue(0.0),
                                   CanonicalValue(0.0), CanonicalValue(1.0)})},
                           })},
    });
    // The primary tool of an interface; written only when true.
    if (const auto *primary = row.find_object_member("primary");
        primary != nullptr && primary->as_bool()) {
      auto edge = attachments.back().as_object();
      edge.emplace_back("primary", CanonicalValue(true));
      attachments.back() = CanonicalValue(std::move(edge));
    }
    // Authored arm-tool pairs that never touch; written only when present.
    if (const auto *exclusions = row.find_object_member("collision_exclusions");
        exclusions != nullptr && !exclusions->as_array().empty()) {
      auto pairs = exclusions->as_array();
      std::sort(pairs.begin(), pairs.end(),
                [](const CanonicalValue &lhs, const CanonicalValue &rhs) {
                  return std::pair(member(lhs, "child_link", "pair").as_string(),
                                   member(lhs, "parent_link", "pair").as_string()) <
                         std::pair(member(rhs, "child_link", "pair").as_string(),
                                   member(rhs, "parent_link", "pair").as_string());
                });
      auto edge = attachments.back().as_object();
      edge.emplace_back("collision_exclusions", CanonicalValue(std::move(pairs)));
      attachments.back() = CanonicalValue(std::move(edge));
    }
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
  // Empty membership keeps the same ROS cell host and optional world.
  // A ROS composition admits any number of robots and components, each
  // of which must select a ROS controller profile from the catalog. Mixing
  // with native components stays refused until the native executor goes.
  const auto has_ros_controller_profile = [](const auto &profile) {
    if (is_model_free_runtime_kind(profile.runtime_kind))
      return true;
    if (profile.runtime_kind != ActivationRuntimeKind::kRos2Control) {
      return false;
    }
    return profile.lane.has_value() || lane_free_tool(profile);
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
      // The published frames include the generated links of the asset's
      // offset frame roles and of the row's custom TCPs.
      auto urdf = prepared_model(index).verified_asset.source_ir().urdf();
      for (const auto &binding :
           prepared_model(index).verified_asset.value().frame_role_bindings) {
        if (binding.pose_in_source_link)
          urdf.links.push_back({binding.frame_role_id.value(), std::nullopt, {}, {}});
      }
      for (const auto &custom : component.custom_tcps)
        urdf.links.push_back({custom.tcp_id, std::nullopt, {}, {}});
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
      configuration = with_lifecycle_budget(configuration, component);
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
                            provider_manifests, component_types));
    }
  }

  Array controller_profiles;
  Array motion_target_limits;
  for (const auto &projection : projections) {
    for (const auto &[name, merged] :
         {std::pair{"controller_profiles", &controller_profiles},
          std::pair{"motion_target_limits", &motion_target_limits}}) {
      const auto *rows = projection.runtime_configuration.find_object_member(name);
      if (rows != nullptr)
        merged->insert(merged->end(), rows->as_array().begin(), rows->as_array().end());
    }
  }
  require_no_exclusive_ros_controller_topic_collision(controller_profiles,
                                                      motion_target_limits);
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

  // Cell-wide runtime fields come from a robot; a tool row carries only its
  // own merged rows (gripper target, streams, controller profile).
  auto physical = std::find_if(
      component_profiles.begin(), component_profiles.end(),
      [](const auto &profile) {
        return !is_model_free_runtime_kind(profile.runtime_kind) &&
               !is_tool(profile);
      });
  if (physical == component_profiles.end())
    physical = std::find_if(
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
        if (is_model_free_runtime_kind(component_profiles[index].runtime_kind) ||
            is_tool(component_profiles[index]))
          continue;
        const auto &projection = projections[index];
        if (member(projection.runtime_configuration, name,
                   "runtime configuration") != expected) {
          fail("grouped native components disagree on " + std::string(name));
        }
      }
    }
    using MergedFields = std::array<std::string_view, 13U>;
    // One ROS runtime unit hosts every component and each component
    // keeps its own controller profile, fault Stop profile, authored
    // gripper target rows and stream defaults; the ROS runtime schema has no
    // native direct-control field.
    const MergedFields merged_fields = MergedFields{
        "initial_joint_positions_rad", "motion_target_limits",
        "default_fault_stop_profiles", "gripper_target_configurations",
        "controller_profiles",         "io_signals",
        "io_provider_devices",         "pose_sources",
        "ai_models",                   "component_streams",
        "lifecycle_command_timeouts",  "machines",
        "sensors"};
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

  const auto compile_graph = [&](const CanonicalValue &configuration) {
    const CanonicalValue graph(Object{
        {"api_version", CanonicalValue(std::string(core::kCellGraphApiVersion))},
        {"graph_revision", CanonicalValue(std::string(kZeroDigest))},
        {"cell_id", CanonicalValue(runtime_cell_id.value())},
        {"runtime_units",
         CanonicalValue(Array{CanonicalValue(Object{
             {"runtime_unit_id", CanonicalValue(runtime_id)},
             {"runtime_profile_id", CanonicalValue(runtime_profile_id)},
             {"runtime_profile_revision", CanonicalValue(revision(runtime_ref))},
             {"dependency_runtime_unit_ids", CanonicalValue(Array{})},
             {"configuration", configuration},
         })})},
        {"model_instances", CanonicalValue(models)},
        {"controllable_components", CanonicalValue(components)},
        {"control_domains", CanonicalValue(domains)},
        {"control_bindings", CanonicalValue(bindings)},
        {"component_attachments", CanonicalValue(attachments)},
    });
    const auto graph_parsed = parse_strict_json_model_yaml(canonical_json(graph));
    const auto typed = decode_cell_graph_candidate(graph_parsed);
    const auto intrinsic = validate_intrinsic_cell_graph_candidate(typed);
    const auto ready = prepare_cell_graph_identity(intrinsic, selected_profiles);
    return compute_graph_revision(ready);
  };
  // The launched descriptions need the composed cell, which needs the
  // graph: compose the graph with the model URDFs, then compile it again with
  // the full descriptions.
  if (!controller_profiles.empty())
    runtime_configuration =
        with_static_frames(runtime_configuration, compile_graph(runtime_configuration),
                           prepared_models, io_types, component_types);
  const auto computed = compile_graph(runtime_configuration);
  const auto &computed_graph = computed.normalized_root();

  Array desired;
  Array providers;
  Array stop_stages;
  for (std::size_t index = 0; index < intent.components.size(); ++index) {
    const auto &component = intent.components[index];
    const auto &profile = component_profiles[index];
    if (is_model_free_runtime_kind(profile.runtime_kind))
      continue;
    for (const std::string_view target : {std::string_view("arm"),
                                          std::string_view("gripper")}) {
      if ((target == "gripper" && !has_gripper_target(component, profile)) ||
          (target == "arm" && is_tool(profile)))
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
      const std::string planning_provider_id(kRos2ControlProviderId);
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
                 CanonicalValue(ros2_control_provider_instance_id(component))},
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
          const auto joints = source_joints(profile, "arm_joints");
          const auto limits = controlled_stop_limits(component, profile, prepared_model(index));
          Array resource_limits;
          for (const auto &resource : array(member(binding, "resource_bindings", "binding"),
                                            "resource bindings")) {
            const auto &resource_id = text(member(resource, "resource_id", "resource"),
                                           "resource_id");
            std::size_t joint_index = 0;
            while (joint_index < joints.size() &&
                   coordinate_resource_id(component.component_id, target, joint_index) !=
                       resource_id) {
              ++joint_index;
            }
            if (joint_index == joints.size())
              fail("arm resource " + resource_id + " has no source joint");
            const auto &joint = joints[joint_index];
            const auto &limit = *limits.at(joint);
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
                fail("controlled Stop max_jerk_rad_s3 for joint " + joint +
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
          // The per-profile maximum Stop time, when authored.
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
    // Every native component uses the generic component lifecycle provider.
    for (const auto capability : kLifecycleCapabilities) {
      static_cast<void>(selector(catalog, capability));
    }
    if (ros2_control) {
      static_cast<void>(selector(catalog, "component.lifecycle.recover"));
    }
  }
  // Model-free provider families: the I/O signal provider, the pose
  // source and the AI model. Each selects its exact maintained manifest once.
  struct ModelFreeProvider final {
    std::string_view manifest_id;
    std::string_view instance_prefix;
    std::string_view binding_prefix;
    std::optional<core::CatalogRecordRefDTO> manifest;
    std::span<const core::ProviderCapabilityOfferDTO> offers;
  };
  ModelFreeProvider io_provider{kIoProviderManifestId,
                                "provider.io.", "io.binding.", {}, {}};
  ModelFreeProvider pose_provider{kPoseSourceProviderManifestId,
                                  "provider.pose_source.",
                                  "pose_source.binding.", {}, {}};
  ModelFreeProvider ai_model_provider{kAiModelProviderManifestId,
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
  // A machine owner, a sensor and the assistant have no targets and no
  // target provider binding.
  const auto target_less = [&](std::string_view component_id) {
    return std::any_of(component_profiles.begin(), component_profiles.end(),
                       [&](const auto &row) {
                         return row.component_id == component_id &&
                                (row.runtime_kind == ActivationRuntimeKind::kMachine ||
                                 row.runtime_kind == ActivationRuntimeKind::kSensor ||
                                 row.runtime_kind == ActivationRuntimeKind::kAssistant);
                       });
  };
  for (const auto &prepared : io_types) {
    if (target_less(prepared.component_id)) continue;
    auto &provider = provider_for(prepared.component_id);
    if (!provider.manifest)
      select_manifest(provider);
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
    if (target_less(prepared.component_id)) continue;
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
  for (auto &row : execution_core_provider_rows(catalog, runtime_cell_id))
    providers.emplace_back(std::move(row));
  // These cell capabilities describe the configured Isaac dependency. They
  // do not create a simulator or change the Runner's control port.
  const bool has_isaac = std::any_of(
      controller_profiles.begin(), controller_profiles.end(), [](const auto &profile) {
        return text(member(profile, "lane", "controller profile"), "lane") == "isaac";
      });
  if (has_isaac) {
    const auto &simulator = catalog.require(
        core::CatalogRecordKind::kProviderManifest, "manyforge.provider.simulator");
    for (const auto capability : kSimulatorCapabilities) {
      std::string normalized(capability);
      std::replace(normalized.begin(), normalized.end(), '.', '-');
      Object row{
          {"capability_provider_binding_id", CanonicalValue(
              runtime_cell_id.value() + "." + normalized + ".provider")},
          {"capability_provider_binding_revision", CanonicalValue(std::string(kZeroDigest))},
      };
      const auto selected_capability = selector(catalog, capability);
      for (const auto &selected : object(selected_capability, "selector"))
        row.push_back(selected);
      row.insert(row.end(), {
          {"scope", CanonicalValue("cell_global")},
          {"target", CanonicalValue(Object{{"cell_id", CanonicalValue(runtime_cell_id.value())}})},
          {"provider_instance_id", CanonicalValue(runtime_id + ".simulator")},
          {"provider_manifest_id", CanonicalValue("manyforge.provider.simulator")},
          {"provider_manifest_revision", CanonicalValue(revision(simulator))},
          {"endpoint_bindings", CanonicalValue(Array{})},
      });
      providers.emplace_back(std::move(row));
    }
  }
  for (std::size_t index = 0; index < component_profiles.size(); ++index)
    if (!is_model_free_runtime_kind(component_profiles[index].runtime_kind) &&
        has_gripper_target(intent.components[index], component_profiles[index])) {
      static_cast<void>(catalog.require(core::CatalogRecordKind::kCommandSchema,
                                        "gripper.command_position"));
      break;
    }
  std::optional<CanonicalValue> assistant;
  if (intent.assistant_profile_id.has_value() != assistant_profiles.runtime.has_value())
    fail("assistant profile has no repository-owned source");
  if (assistant_profiles.runtime || assistant_profiles.chat) {
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
    assistant = assistant_member(assistant_profiles,
                                 intent.assistant_model_components, ai_models);
    // A usable model query: a prompt template and a runtime model alias
    // (every alias is bound to an AI model component above).
    if (assistant &&
        !object(member(*assistant, "prompt_templates", "assistant"),
                "prompt_templates").empty() &&
        std::any_of(object(member(*assistant, "model_endpoint_aliases", "assistant"),
                           "model_endpoint_aliases").begin(),
                    object(member(*assistant, "model_endpoint_aliases", "assistant"),
                           "model_endpoint_aliases").end(),
                    [](const auto &row) { return row.first != kChatModelAlias; })) {
      const auto &model_query = catalog.require(
          core::CatalogRecordKind::kProviderManifest,
          "manyforge.provider.assistant_model_query");
      Object row{
          {"capability_provider_binding_id", CanonicalValue(
              runtime_cell_id.value() + ".assistant-model-query.provider")},
          {"capability_provider_binding_revision", CanonicalValue(std::string(kZeroDigest))},
      };
      const auto selected_capability = selector(catalog, "assistant.model.query");
      for (const auto &selected : object(selected_capability, "selector"))
        row.push_back(selected);
      row.insert(row.end(), {
          {"scope", CanonicalValue("cell_global")},
          {"target", CanonicalValue(Object{{"cell_id", CanonicalValue(runtime_cell_id.value())}})},
          {"provider_instance_id", CanonicalValue(runtime_cell_id.value() + ".assistant_model_query")},
          {"provider_manifest_id", CanonicalValue("manyforge.provider.assistant_model_query")},
          {"provider_manifest_revision", CanonicalValue(revision(model_query))},
          {"endpoint_bindings", CanonicalValue(Array{})},
      });
      providers.emplace_back(std::move(row));
    }
      // The chat: the assistant component provides the bridge request (its
    // endpoint is published at runtime in its state, role
    // `assistant_request`) and its lifecycle.
    if (assistant_profiles.chat) {
      const auto &bridge = catalog.require(core::CatalogRecordKind::kProviderManifest,
                                           "manyforge.provider.assistant_bridge");
      for (const auto capability :
           {"component.lifecycle.observe", "component.lifecycle.prepare",
            "component.lifecycle.enable", "component.lifecycle.disable",
            "component.lifecycle.recover"})
        static_cast<void>(selector(catalog, capability));
      Object row{
          {"capability_provider_binding_id", CanonicalValue(
              runtime_cell_id.value() + ".assistant-bridge-request.provider")},
          {"capability_provider_binding_revision", CanonicalValue(std::string(kZeroDigest))},
      };
      const auto selected_capability = selector(catalog, "assistant.bridge.request");
      for (const auto &selected : object(selected_capability, "selector"))
        row.push_back(selected);
      row.insert(row.end(), {
          {"scope", CanonicalValue("cell_global")},
          {"target", CanonicalValue(Object{{"cell_id", CanonicalValue(runtime_cell_id.value())}})},
          {"provider_instance_id", CanonicalValue(runtime_cell_id.value() + ".assistant_bridge")},
          {"provider_manifest_id", CanonicalValue("manyforge.provider.assistant_bridge")},
          {"provider_manifest_revision", CanonicalValue(revision(bridge))},
          {"endpoint_bindings", CanonicalValue(Array{})},
      });
      providers.emplace_back(std::move(row));
    }
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
  if (assistant.has_value())
    root.emplace_back("assistant", std::move(*assistant));
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
