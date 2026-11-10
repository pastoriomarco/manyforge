#include "activation_component_profile_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <manyforge_core/types/typed_error.hpp>
#include <manyforge_deployment/artifact_codec.hpp>

namespace manyforge::deployment::detail {
namespace {

using Array = CanonicalValue::Array;
using Object = CanonicalValue::Object;

constexpr std::string_view kDescriptionApiVersion =
    "manyforge.maintained_deployment_sources.v1";
constexpr std::string_view kDescriptionRelativePath =
    "manyforge_deployment/resources/maintained/"
    "maintained_deployment_sources.v1.json";
constexpr std::string_view kAssistantProfilesApiVersion =
    "manyforge.maintained_assistant_profiles.v2";
constexpr std::string_view kAssistantProfilesRelativePath =
    "manyforge_deployment/resources/maintained/"
    "maintained_assistant_profiles.v2.json";
constexpr std::size_t kMaximumModelFamilies = 128U;

[[noreturn]] void fail(std::string message) {
  throw std::invalid_argument("Activation component profiles " +
                              std::move(message));
}

[[nodiscard]] const Object &object(const CanonicalValue &value,
                                   std::string_view label) {
  const auto *result = std::get_if<Object>(&value.storage());
  if (result == nullptr)
    fail(std::string(label) + " must be an object");
  return *result;
}

[[nodiscard]] const Array &array(const CanonicalValue &value,
                                 std::string_view label) {
  const auto *result = std::get_if<Array>(&value.storage());
  if (result == nullptr)
    fail(std::string(label) + " must be an array");
  return *result;
}

[[nodiscard]] const CanonicalValue &member(const CanonicalValue &value,
                                           std::string_view name,
                                           std::string_view label) {
  static_cast<void>(object(value, label));
  const auto *result = value.find_object_member(name);
  if (result == nullptr) {
    fail(std::string(label) + " is missing " + std::string(name));
  }
  return *result;
}

[[nodiscard]] const std::string &text(const CanonicalValue &value,
                                      std::string_view label) {
  const auto *result = std::get_if<std::string>(&value.storage());
  if (result == nullptr || result->empty()) {
    fail(std::string(label) + " must be non-empty text");
  }
  return *result;
}

void require_fields(const CanonicalValue &value,
                    std::initializer_list<std::string_view> expected,
                    std::string_view label) {
  std::set<std::string, std::less<>> remaining(expected.begin(),
                                               expected.end());
  for (const auto &[name, unused] : object(value, label)) {
    static_cast<void>(unused);
    if (remaining.erase(name) == 0U) {
      fail(std::string(label) + " has unexpected field " + name);
    }
  }
  if (!remaining.empty()) {
    fail(std::string(label) + " is missing " + *remaining.begin());
  }
}

[[nodiscard]] std::string
read_description(const std::filesystem::path &capability_root,
                 std::string_view relative_path = kDescriptionRelativePath) {
  const auto path = capability_root / relative_path;
  if (std::filesystem::is_symlink(path) ||
      !std::filesystem::is_regular_file(path)) {
    fail("description must select one plain file: " + path.string());
  }
  const auto size = std::filesystem::file_size(path);
  if (size == 0U || size > kMaxArtifactEncodedBytes) {
    fail("description is empty or exceeds its byte bound: " + path.string());
  }
  std::ifstream input(path, std::ios::binary);
  if (!input)
    fail("cannot open description: " + path.string());
  std::string bytes(static_cast<std::size_t>(size), '\0');
  input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!input || input.peek() != std::char_traits<char>::eof()) {
    fail("description changed while it was read: " + path.string());
  }
  return bytes;
}

struct Family final {
  std::string model_asset_id;
  std::string accepted_package_uri_prefix;
  CanonicalValue value;
};

/// One component model: its descriptor (`component.yaml`) and the family its
/// maintained row names.
struct Model final {
  std::string model_family_id;
  CanonicalValue descriptor;
};

[[nodiscard]] ActivationRuntimeKind
parse_runtime_kind(const CanonicalValue &configuration,
                   std::string_view label) {
  const auto &value = text(member(configuration, "runtime_kind", label),
                           std::string(label) + ".runtime_kind");
  if (value == "ros2_control")
    return ActivationRuntimeKind::kRos2Control;
  if (value == "io_provider")
    return ActivationRuntimeKind::kIoProvider;
  if (value == "pose_source")
    return ActivationRuntimeKind::kPoseSource;
  if (value == "ai_model")
    return ActivationRuntimeKind::kAiModel;
  if (value == "sensor")
    return ActivationRuntimeKind::kSensor;
  if (value == "assistant")
    return ActivationRuntimeKind::kAssistant;
  fail(std::string(label) + ".runtime_kind is unsupported: " + value);
}

[[nodiscard]] std::string_view
runtime_kind_text(ActivationRuntimeKind kind) noexcept {
  switch (kind) {
  case ActivationRuntimeKind::kIoProvider:
    return "io_provider";
  case ActivationRuntimeKind::kPoseSource:
    return "pose_source";
  case ActivationRuntimeKind::kAiModel:
    return "ai_model";
  case ActivationRuntimeKind::kMachine:
    return "machine";
  case ActivationRuntimeKind::kSensor:
    return "sensor";
  case ActivationRuntimeKind::kAssistant:
    return "assistant";
  case ActivationRuntimeKind::kRos2Control:
    break;
  }
  return "ros2_control";
}

/// The model-free component profiles: no model, no physical type.
struct ModelFreeProfile final {
  std::string_view profile_id;
  ActivationRuntimeKind kind;
};
constexpr ModelFreeProfile kModelFreeProfiles[] = {
    {"io_device_v0", ActivationRuntimeKind::kIoProvider},
    {"pose_source_v0", ActivationRuntimeKind::kPoseSource},
    {"ai_model_v0", ActivationRuntimeKind::kAiModel},
};

[[nodiscard]] const ModelFreeProfile *
model_free_profile(std::string_view profile_id) {
  for (const auto &candidate : kModelFreeProfiles)
    if (candidate.profile_id == profile_id) return &candidate;
  return nullptr;
}

/// A lane-free tool: a pneumatic gripper whose lane lives in the I/O
/// components its `pneumatic_io` references, so it names no lane.
[[nodiscard]] bool may_be_lane_free(const CanonicalValue &descriptor) {
  const auto *kind = descriptor.find_object_member("kind");
  const auto *gripper = descriptor.find_object_member("gripper");
  const auto *actuation =
      gripper != nullptr ? gripper->find_object_member("actuation") : nullptr;
  return kind != nullptr && kind->is_string() && kind->as_string() == "tool" &&
         actuation != nullptr && actuation->is_string() &&
         actuation->as_string() == "pneumatic";
}

} // namespace

std::vector<ActivationComponentProfile> resolve_activation_component_profiles(
    const std::filesystem::path &capability_root,
    const ActivationDeploymentIntent &intent) {
  const auto parsed =
      parse_strict_json_model_yaml(read_description(capability_root));
  const auto &root = parsed.root();
  // `component_models`: the descriptors of models without a model family
  // (machines and model-form I/O devices; sensors).
  if (root.find_object_member("component_models") != nullptr)
    require_fields(root, {"api_version", "component_models", "model_families"},
                   "description");
  else
    require_fields(root, {"api_version", "model_families"}, "description");
  if (text(member(root, "api_version", "description"),
           "description.api_version") != kDescriptionApiVersion) {
    fail("description has an unsupported api_version");
  }

  const auto &family_rows = array(member(root, "model_families", "description"),
                                  "description.model_families");
  if (family_rows.empty() || family_rows.size() > kMaximumModelFamilies) {
    fail("description has an invalid model-family count");
  }
  std::map<std::string, Family, std::less<>> families;
  std::map<std::string, Model, std::less<>> models;
  std::set<std::string, std::less<>> model_asset_ids;
  for (std::size_t index = 0U; index < family_rows.size(); ++index) {
    const auto &row = family_rows[index];
    const std::string label =
        "description.model_families[" + std::to_string(index) + "]";
    // A family selectable by a component names its model descriptor
    // (`component_descriptor`); a descriptor-registered family may also
    // record its TCPs. Families without a descriptor are test fixtures.
    std::set<std::string_view> fields{"catalog_source", "model_asset_id",
                                      "model_family_id", "semantics", "source"};
    for (const std::string_view optional :
         {"component_descriptor", "tcp_frame_role_ids"})
      if (row.find_object_member(optional) != nullptr) fields.insert(optional);
    for (const auto &[name, unused] : object(row, label)) {
      static_cast<void>(unused);
      if (!fields.contains(name)) fail(label + " has unexpected field " + name);
    }
    for (const auto name : fields)
      static_cast<void>(member(row, name, label));
    const auto &source = member(row, "source", label);
    require_fields(source,
                   {"accepted_package_uri_prefix", "historical_srdf_path",
                    "historical_urdf_path", "resource_root", "srdf_path",
                    "srdf_projection_kind", "urdf_path",
                    "urdf_projection_kind"},
                   label + ".source");
    static_cast<void>(
        object(member(row, "semantics", label), label + ".semantics"));
    const auto &family_id =
        text(member(row, "model_family_id", label), label + ".model_family_id");
    const auto &model_asset_id =
        text(member(row, "model_asset_id", label), label + ".model_asset_id");
    if (!model_asset_ids.insert(model_asset_id).second ||
        !families
             .emplace(
                 family_id,
                 Family{model_asset_id,
                        text(member(source, "accepted_package_uri_prefix",
                                    label + ".source"),
                             label + ".source.accepted_package_uri_prefix"),
                        row})
             .second) {
      fail("description has duplicate model-family or model-asset identities");
    }
    if (const auto *path = row.find_object_member("component_descriptor")) {
      const auto descriptor = parse_strict_json_model_yaml(read_description(
          capability_root, text(*path, label + ".component_descriptor")));
      const auto &model_id =
          text(member(descriptor.root(), "id", label + " descriptor"),
               label + " descriptor.id");
      static_cast<void>(object(member(descriptor.root(), "lanes",
                                      label + " descriptor"),
                               label + " descriptor.lanes"));
      if (!models.emplace(model_id, Model{family_id, descriptor.root()}).second)
        fail("description has duplicate component model " + model_id);
    }
  }

  if (const auto *paths = root.find_object_member("component_models")) {
    for (const auto &path : array(*paths, "description.component_models")) {
      const auto descriptor = parse_strict_json_model_yaml(read_description(
          capability_root, text(path, "description.component_models[]")));
      const auto &model_id = text(member(descriptor.root(), "id", "component model"),
                                  "component model id");
      const auto &kind = text(member(descriptor.root(), "kind", "component model"),
                              "component model kind");
      if (kind != "machine" && kind != "io_device" && kind != "sensor" &&
          kind != "assistant")
        fail("component model " + model_id +
             " must be a machine, an I/O device, a sensor or an assistant");
      // A sensor or an assistant has no lanes: one adapter row on every lane.
      if (kind != "sensor" && kind != "assistant")
        static_cast<void>(object(member(descriptor.root(), "lanes", "component model"),
                                 "component model lanes"));
      if (!models.emplace(model_id, Model{std::string{}, descriptor.root()}).second)
        fail("description has duplicate component model " + model_id);
    }
  }
  const auto model_kind = [](const CanonicalValue &descriptor) {
    const auto *kind = descriptor.find_object_member("kind");
    return kind != nullptr && kind->is_string() ? kind->as_string() : std::string{};
  };
  const auto component_of = [&](std::string_view id) {
    const auto found = std::find_if(
        intent.components.begin(), intent.components.end(),
        [&](const auto &row) { return row.component_id == id; });
    if (found == intent.components.end())
      throw std::logic_error("parsed member lost its owner");
    return &*found;
  };
  // The machine's member slot of `role`, or null.
  const auto slot = [&](const CanonicalValue &machine,
                        std::string_view role) -> const CanonicalValue * {
    for (const auto &row : array(member(machine, "members", "machine model"),
                                 "machine model members"))
      if (text(member(row, "role", "machine member"), "machine member role") == role)
        return &row;
    return nullptr;
  };
  // Slot checks per owner, over its active and inactive members.
  {
    std::map<std::string, std::set<std::string, std::less<>>, std::less<>> roles;
    const auto check_member = [&](std::string_view component_id,
                                  std::string_view profile_id,
                                  const std::string &owner,
                                  const std::string &role) {
      const auto *owner_row = component_of(owner);
      const auto machine = models.find(owner_row->component_profile_id);
      if (machine == models.end() || model_kind(machine->second.descriptor) != "machine")
        fail("component " + owner + " is an owner but selects no machine model");
      const auto *selected = slot(machine->second.descriptor, role);
      if (selected == nullptr ||
          text(member(*selected, "model", "machine member"), "machine member model") !=
              profile_id)
        throw core::TypedError(
            "deployment_member_role_unknown",
            "Activation component profiles: deployment_member_role_unknown: " +
                std::string(component_id) + " (model " + std::string(profile_id) +
                ") isn't a " + role + " member of " +
                owner_row->component_profile_id);
      if (!roles[owner].insert(role).second)
        throw core::TypedError(
            "deployment_member_role_repeated",
            "Activation component profiles: deployment_member_role_repeated: " +
                owner + " has more than one " + role + " member");
    };
    for (const auto &component : intent.components)
      if (component.owner)
        check_member(component.component_id, component.component_profile_id,
                     *component.owner, *component.role);
    for (const auto &inactive : intent.inactive_members)
      check_member(inactive.component_id, inactive.component_profile_id,
                   inactive.owner, inactive.role);
    // A machine's members come from its model and are
    // always all present; a member is only deactivated, never removed.
    for (const auto &component : intent.components) {
      const auto machine = models.find(component.component_profile_id);
      if (component.owner || machine == models.end() ||
          model_kind(machine->second.descriptor) != "machine")
        continue;
      for (const auto &slot_row : array(member(machine->second.descriptor, "members",
                                               "machine model"),
                                        "machine model members")) {
        const auto &role = text(member(slot_row, "role", "machine member"),
                                "machine member role");
        if (!roles[component.component_id].contains(role))
          throw core::TypedError(
              "deployment_member_missing",
              "Activation component profiles: deployment_member_missing: " +
                  component.component_id + " has no " + role +
                  " member; deactivate a member instead of removing it");
      }
    }
  }
  // A machine's member model (its arm, an I/O bank) is placed only as
  // a member of that machine, on every lane.
  for (const auto &[machine_id, machine] : models) {
    if (model_kind(machine.descriptor) != "machine") continue;
    for (const auto &slot_row : array(member(machine.descriptor, "members", "machine model"),
                                      "machine model members")) {
      const auto &slot_model = text(member(slot_row, "model", "machine member"),
                                    "machine member model");
      for (const auto &component : intent.components)
        if (!component.owner && component.component_profile_id == slot_model)
          throw core::TypedError(
              "deployment_machine_part_without_machine",
              "Activation component profiles: deployment_machine_part_without_machine: " +
                  component.component_id + " (model " + slot_model + ") is a part of " +
                  machine_id + "; place it as a member of that machine");
    }
  }

  std::vector<ActivationComponentProfile> result;
  result.reserve(intent.components.size());
  for (const auto &component : intent.components) {
    const auto found_model = models.find(component.component_profile_id);
    const auto kind = found_model == models.end()
                          ? std::string{}
                          : model_kind(found_model->second.descriptor);
    if (kind == "sensor" || kind == "assistant") {
      // A sensor or the assistant: no owner, no model asset, no lane; one
      // adapter row (the model's `adapter_row`).
      const auto lane_free = kind == "sensor" ? ActivationRuntimeKind::kSensor
                                              : ActivationRuntimeKind::kAssistant;
      if (component.owner)
        fail("component " + component.component_id + " selects " + kind + " model " +
             component.component_profile_id + " but is a member");
      const auto authored_kind = parse_runtime_kind(
          component.configuration,
          "component " + component.component_id + ".configuration");
      if (authored_kind != lane_free)
        fail("component " + component.component_id + " selects " +
             std::string(runtime_kind_text(authored_kind)) +
             " runtime configuration for " + kind + " model " +
             component.component_profile_id);
      result.push_back(ActivationComponentProfile{
          component.component_id, component.component_profile_id, {}, {}, {},
          lane_free, CanonicalValue(nullptr),
          found_model->second.descriptor, std::nullopt, std::nullopt});
      continue;
    }
    if (kind == "machine" || kind == "io_device") {
      const auto &descriptor = found_model->second.descriptor;
      const auto *owner_row =
          component.owner ? component_of(*component.owner) : &component;
      if ((kind == "machine") == component.owner.has_value())
        fail("component " + component.component_id + " selects " + kind +
             " model " + component.component_profile_id +
             (kind == "machine" ? " but is a member" : " but has no owner"));
      // One machine, one lane: the owner's.
      const auto &lane = *owner_row->lane;
      std::optional<CanonicalValue> lane_row;
      if (const auto *row =
              member(descriptor, "lanes", "component model").find_object_member(lane))
        lane_row = *row;
      else if (kind == "io_device" && lane == "real")
        throw core::TypedError(
            "component_lane_unknown",
            "Activation component profiles: component_lane_unknown: " +
                component.component_id + " lane real is not a lane of model " +
                component.component_profile_id);
      ActivationComponentProfile profile{
          component.component_id, component.component_profile_id, {}, {}, {},
          kind == "machine" ? ActivationRuntimeKind::kMachine
                            : ActivationRuntimeKind::kIoProvider,
          CanonicalValue(nullptr), descriptor, lane, std::move(lane_row)};
      profile.owner_component_id = component.owner;
      profile.member_role = component.role;
      if (const auto *ref = owner_row->configuration.find_object_member(
              "device_address_ref"))
        profile.owner_device_address_ref = *ref;
      // On a lane where the machine has a process, a
      // deactivated arm (the member with a description) still gives it its
      // lane row, description and joints.
      for (const auto &inactive : intent.inactive_members) {
        if (kind != "machine" || !profile.lane_row ||
            inactive.owner != component.component_id || !inactive.description)
          continue;
        const auto arm = models.find(inactive.component_profile_id);
        if (arm == models.end())
          fail("component " + inactive.component_id + " selects unknown model " +
               inactive.component_profile_id);
        const auto *arm_row = member(arm->second.descriptor, "lanes", "component model")
                                  .find_object_member(lane);
        if (arm_row == nullptr)
          throw core::TypedError(
              "component_lane_unknown",
              "Activation component profiles: component_lane_unknown: " +
                  inactive.component_id + " lane " + lane + " is not a lane of model " +
                  inactive.component_profile_id);
        const auto family = families.find(arm->second.model_family_id);
        if (family == families.end())
          throw std::logic_error("validated component model lost its family");
        std::vector<std::string> joints;
        for (const auto &binding :
             array(member(member(family->second.value, "semantics", "model family"),
                          "resource_role_bindings", "model semantics"),
                   "resource_role_bindings"))
          if (text(member(binding, "resource_role_id", "resource binding"),
                   "resource_role_id") == "arm_joints")
            for (const auto &joint : array(member(binding, "source_joint_names",
                                                  "resource binding"),
                                           "source_joint_names"))
              joints.push_back(text(joint, "source joint"));
        if (joints.empty())
          fail("component " + inactive.component_id + " (model " +
               inactive.component_profile_id + ") has no arm joints");
        profile.deactivated_arm = ActivationComponentProfile::DeactivatedArm{
            *arm_row, arm->second.model_family_id, *inactive.description, std::move(joints)};
      }
      result.push_back(std::move(profile));
      continue;
    }
    const auto authored_kind = parse_runtime_kind(
        component.configuration,
        "component " + component.component_id + ".configuration");
    const auto *model_free = model_free_profile(component.component_profile_id);
    const auto model = models.find(component.component_profile_id);
    if (model_free == nullptr && model == models.end()) {
      fail("component " + component.component_id + " selects unknown model " +
           component.component_profile_id);
    }
    const auto selected_kind = model_free != nullptr
                                   ? model_free->kind
                                   : ActivationRuntimeKind::kRos2Control;
    if (authored_kind != selected_kind) {
      fail("component " + component.component_id + " selects " +
           std::string(runtime_kind_text(authored_kind)) +
           " runtime configuration for " +
           std::string(runtime_kind_text(selected_kind)) + " profile " +
           component.component_profile_id);
    }
    if (model_free != nullptr) {
      if (component.lane.has_value())
        throw core::TypedError(
            "component_lane_unexpected",
            "Activation component profiles: component_lane_unexpected: " +
                component.component_id + " is model-free but names lane " +
                *component.lane);
      if (component.model_source || component.root_pose_in_cell ||
          !component.frame_prefix.empty())
        fail("model-free profile carries physical model fields");
      result.push_back(ActivationComponentProfile{
          component.component_id, component.component_profile_id, {}, {}, {},
          selected_kind, CanonicalValue(nullptr), CanonicalValue(nullptr),
          std::nullopt, std::nullopt});
      continue;
    }
    // The authored lane picks one of the model's `lanes` rows; only a
    // pneumatic tool may name none (lane-free). A member runs on its
    // owner's lane.
    const auto &descriptor = model->second.descriptor;
    const auto &lanes = member(descriptor, "lanes", "component model");
    const auto *owner_row =
        component.owner ? component_of(*component.owner) : nullptr;
    const auto lane = owner_row != nullptr ? owner_row->lane : component.lane;
    std::optional<CanonicalValue> lane_row;
    if (lane.has_value()) {
      const auto *row = lanes.find_object_member(*lane);
      if (row == nullptr)
        throw core::TypedError(
            "component_lane_unknown",
            "Activation component profiles: component_lane_unknown: " +
                component.component_id + " lane " + *lane +
                " is not a lane of model " + component.component_profile_id);
      static_cast<void>(object(*row, "component model lane row"));
      lane_row = *row;
    } else if (!may_be_lane_free(descriptor)) {
      throw core::TypedError(
          "component_lane_required",
          "Activation component profiles: component_lane_required: " +
              component.component_id + " (model " +
              component.component_profile_id + ") must name its lane");
    }
    const auto family = families.find(model->second.model_family_id);
    if (family == families.end()) {
      throw std::logic_error("validated component model lost its family");
    }
    ActivationComponentProfile profile{
        component.component_id,
        component.component_profile_id,
        model->second.model_family_id,
        family->second.model_asset_id,
        family->second.accepted_package_uri_prefix,
        authored_kind,
        family->second.value,
        descriptor,
        lane,
        std::move(lane_row),
    };
    if (owner_row != nullptr) {
      profile.owner_component_id = component.owner;
      profile.member_role = component.role;
      if (const auto *ref =
              owner_row->configuration.find_object_member("device_address_ref"))
        profile.owner_device_address_ref = *ref;
    }
    result.push_back(std::move(profile));
  }
  return result;
}

void expand_activation_machine_members(
    ActivationDeploymentIntent &intent,
    const std::vector<ActivationComponentProfile> &profiles) {
  if (intent.components.size() != profiles.size())
    throw std::logic_error("machine member expansion joins differ in size");
  for (std::size_t index = 0U; index != profiles.size(); ++index) {
    const auto &profile = profiles[index];
    if (profile.runtime_kind != ActivationRuntimeKind::kIoProvider ||
        !profile.member_role.has_value())
      continue;
    auto &component = intent.components[index];
    const auto &id = component.component_id;
    // Real: the vendor family serves the bank through the owner's
    // connection; Mock and Isaac: the host's Mock I/O.
    const bool adapter = *profile.lane == "real";
    Array signals;
    for (const auto &signal :
         array(member(profile.model_descriptor, "signals", "I/O model"),
               "I/O model signals")) {
      const auto &name = text(member(signal, "signal_name", "I/O signal"),
                              "I/O signal name");
      const bool output =
          text(member(signal, "direction", "I/O signal"), "direction") == "output";
      Object row{
          {"controllable_component_id", CanonicalValue(id)},
          {"command_target_id", CanonicalValue("io")},
          {"resource_role_id", CanonicalValue(name)},
          {"signal_name", CanonicalValue(name)},
          {"source_id", CanonicalValue(id + "." + name)},
          {"value_type", CanonicalValue("bool")},
          {"direction", CanonicalValue(output ? "output" : "input")},
      };
      Object provider;
      if (adapter) {
        provider = {{"profile", CanonicalValue("component_adapter")},
                    {"bank", member(*profile.lane_row, "bank", "I/O lane row")}};
        if (!output) provider.emplace_back("ingress_depth", CanonicalValue(16.0));
      } else if (output) {
        provider = {{"profile", CanonicalValue("mock_output")},
                    {"channel", CanonicalValue(id + "." + name)}};
      } else {
        provider = {{"profile", CanonicalValue("mock")},
                    {"initial_value", CanonicalValue(false)},
                    {"ingress_depth", CanonicalValue(64.0)}};
      }
      row.emplace_back("provider", CanonicalValue(std::move(provider)));
      if (!output) {
        row.emplace_back("update_mode",
                         CanonicalValue(adapter ? "sampled" : "change_only"));
        row.emplace_back("transition_capability", CanonicalValue("sampled_only"));
        // Real: the bank reports its inputs once per stream period, so they
        // carry the model's sample-age budget; a stale input leaves a rule
        // unable to watch it.
        Object freshness{{"mode", CanonicalValue("provider_validity")}};
        if (adapter)
          freshness = {
              {"maximum_age_ns",
               CanonicalValue(std::ceil(
                   member(*profile.lane_row, "input_maximum_age_ms", "I/O lane row")
                       .as_number() *
                   1e6))},
              {"mode", CanonicalValue("sample_age")}};
        row.emplace_back("freshness", CanonicalValue(std::move(freshness)));
      }
      signals.emplace_back(std::move(row));
    }
    component.configuration = CanonicalValue(Object{
        {"runtime_kind", CanonicalValue("io_provider")},
        {"runtime_configuration",
         CanonicalValue(Object{
             {"io_provider_devices",
              CanonicalValue(Array{CanonicalValue(Object{
                  {"controllable_component_id", CanonicalValue(id)},
                  {"member_role", CanonicalValue(*profile.member_role)},
                  {"owner_component_id",
                   CanonicalValue(*profile.owner_component_id)},
              })})},
             {"io_signals", CanonicalValue(std::move(signals))},
         })},
    });
  }
}

ActivationAssistantProfiles resolve_activation_assistant_profiles(
    const std::filesystem::path &capability_root,
    const ActivationDeploymentIntent &intent,
    const std::vector<ActivationComponentProfile> &profiles) {
  if (intent.components.size() != profiles.size())
    throw std::invalid_argument("assistant component profile join differs");
  std::optional<std::size_t> assistant;
  for (std::size_t index = 0; index != profiles.size(); ++index) {
    if (profiles[index].runtime_kind != ActivationRuntimeKind::kAssistant)
      continue;
    // One chat per cell (the AI tab has one).
    if (assistant)
      throw core::TypedError(
          "assistant_component_duplicate",
          "Activation component profiles: assistant_component_duplicate: " +
              intent.components[index].component_id + " is a second assistant component (" +
              intent.components[*assistant].component_id + " is the first)",
          {{"component_id", intent.components[index].component_id}});
    assistant = index;
  }
  if (!intent.assistant_profile_id && !assistant) return {};
  const auto parsed = parse_strict_json_model_yaml(
      read_description(capability_root, kAssistantProfilesRelativePath));
  const auto &root = parsed.root();
  require_fields(root, {"api_version", "mode_profiles", "profiles"},
                 "assistant profiles");
  if (text(member(root, "api_version", "assistant profiles"),
           "assistant profiles.api_version") != kAssistantProfilesApiVersion)
    fail("assistant profiles have an unsupported api_version");
  const auto find = [&](const std::string &profile_id) {
    for (const auto &row : array(member(root, "profiles", "assistant profiles"),
                                 "assistant profiles.profiles")) {
      if (text(member(row, "profile_id", "assistant profile"),
               "assistant profile.profile_id") != profile_id)
        continue;
      const auto &mode_profile_id =
          member(row, "mode_profile_id", "assistant profile");
      if (mode_profile_id.is_null())
        return ActivationAssistantProfile{row, CanonicalValue(Object{})};
      return ActivationAssistantProfile{
          row, member(member(root, "mode_profiles", "assistant profiles"),
                      text(mode_profile_id, "assistant profile.mode_profile_id"),
                      "assistant profiles.mode_profiles")};
    }
    fail("assistant profile is unknown: " + profile_id);
  };
  ActivationAssistantProfiles result;
  if (intent.assistant_profile_id)
    result.runtime = find(*intent.assistant_profile_id);
  if (assistant) {
    const auto &component = intent.components[*assistant];
    const auto label = "component " + component.component_id;
    const auto &settings = member(component.configuration, "runtime_configuration",
                                  label + ".configuration");
    require_fields(settings, {"assistant_profile_id", "model_component_id"},
                   label + ".runtime_configuration");
    auto chat = find(text(member(settings, "assistant_profile_id", label),
                          label + ".assistant_profile_id"));
    if (object(chat.modes, "assistant modes").empty())
      throw core::TypedError(
          "assistant_profile_has_no_chat_mode",
          "Activation component profiles: assistant_profile_has_no_chat_mode: " +
              component.component_id + " selects assistant profile " +
              text(member(settings, "assistant_profile_id", label), "assistant_profile_id") +
              ", which has no chat mode",
          {{"component_id", component.component_id}});
    result.chat = ActivationAssistantChat{
        component.component_id,
        text(member(settings, "model_component_id", label), label + ".model_component_id"),
        std::move(chat)};
  }
  return result;
}

} // namespace manyforge::deployment::detail
