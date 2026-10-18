#include "activation_deployment_intent_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace manyforge::deployment::detail {
namespace {

using Array = CanonicalValue::Array;
using Object = CanonicalValue::Object;

constexpr std::string_view kApiVersion = "manyforge.deployment_intent.v2";
constexpr std::size_t kMaximumComponents = 128U;
constexpr std::size_t kMaximumAttachments = 256U;
constexpr std::size_t kMaximumPackageRoots = 128U;
constexpr std::size_t kMaximumTargets = 128U;

[[noreturn]] void fail(std::string message) {
  throw std::invalid_argument("Deployment intent " + std::move(message));
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

[[nodiscard]] const CanonicalValue &member(const CanonicalValue &value,
                                           std::string_view name,
                                           std::string_view label) {
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
                    std::initializer_list<std::string_view> required,
                    std::initializer_list<std::string_view> optional,
                    std::string_view label) {
  const std::set<std::string, std::less<>> required_set(required.begin(),
                                                        required.end());
  std::set<std::string, std::less<>> allowed(required_set);
  allowed.insert(optional.begin(), optional.end());
  std::set<std::string, std::less<>> actual;
  for (const auto &[name, unused] : object(value, label)) {
    static_cast<void>(unused);
    actual.insert(name);
  }
  for (const auto &name : required_set) {
    if (!actual.contains(name)) {
      fail(std::string(label) + " is missing " + name);
    }
  }
  for (const auto &name : actual) {
    if (!allowed.contains(name)) {
      fail(std::string(label) + " has unexpected field " + name);
    }
  }
}

[[nodiscard]] bool portable_id(std::string_view value) noexcept {
  if (value.empty() || value.size() > 128U || value.front() < 'a' ||
      value.front() > 'z') {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9') || character == '.' ||
           character == '_' || character == '-';
  });
}

[[nodiscard]] const std::string &portable_identifier(
    const CanonicalValue &value, std::string_view label) {
  const auto &result = text(value, label);
  if (!portable_id(result)) {
    fail(std::string(label) + " must be a portable identifier");
  }
  return result;
}

[[nodiscard]] bool valid_frame_prefix(std::string_view value) noexcept {
  if (value.empty()) return true;
  if (value.front() == '/' || value.back() != '/') return false;
  return std::all_of(value.begin(), value.end(), [](char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '_' ||
           character == '/';
  });
}

void require_source_reference(const CanonicalValue &value,
                              std::string_view label) {
  const auto &members = object(value, label);
  if (members.size() != 1U ||
      (members.front().first != "project_path" &&
       members.front().first != "external_path")) {
    fail(std::string(label) +
         " must contain exactly project_path or external_path");
  }
  static_cast<void>(text(members.front().second, label));
}

void require_model_source(const CanonicalValue &value, bool captured_has_srdf,
                          std::string_view label) {
  require_fields(value, {"package_roots", "urdf"}, {"srdf"}, label);
  require_source_reference(member(value, "urdf", label),
                           std::string(label) + ".urdf");
  const auto *srdf = value.find_object_member("srdf");
  if ((srdf != nullptr) != captured_has_srdf) {
    fail(std::string(label) +
         " differs from the captured SRDF presence");
  }
  if (srdf != nullptr) {
    require_source_reference(*srdf, std::string(label) + ".srdf");
  }
  const auto &roots = object(member(value, "package_roots", label),
                             std::string(label) + ".package_roots");
  if (roots.size() > kMaximumPackageRoots) {
    fail(std::string(label) + ".package_roots exceeds its bound");
  }
  for (const auto &[package_name, reference] : roots) {
    if (!portable_id(package_name) || package_name.find('.') != std::string::npos) {
      fail(std::string(label) + ".package_roots has an invalid package name");
    }
    require_source_reference(
        reference, std::string(label) + ".package_roots." + package_name);
  }
}

void require_pose(const CanonicalValue &value, std::string_view label) {
  require_fields(value, {"orientation_xyzw", "position_m"}, {}, label);
  const auto require_coordinates = [&](std::string_view name,
                                       std::size_t expected) {
    const auto &coordinates =
        array(member(value, name, label), std::string(label) + "." +
                                           std::string(name));
    if (coordinates.size() != expected) {
      fail(std::string(label) + "." + std::string(name) +
           " has the wrong coordinate count");
    }
    double norm = 0.0;
    for (const auto &coordinate : coordinates) {
      const auto *number = std::get_if<double>(&coordinate.storage());
      if (number == nullptr || !std::isfinite(*number)) {
        fail(std::string(label) + "." + std::string(name) +
             " must contain finite numbers");
      }
      norm += *number * *number;
    }
    return norm;
  };
  static_cast<void>(require_coordinates("position_m", 3U));
  if (require_coordinates("orientation_xyzw", 4U) <= 1e-24) {
    fail(std::string(label) + ".orientation_xyzw must be non-zero");
  }
}

void require_target_settings(const CanonicalValue &value,
                             std::string_view label) {
  const auto &settings = object(value, label);
  if (settings.size() > kMaximumTargets) {
    fail(std::string(label) + " exceeds its target bound");
  }
  for (const auto &[target_id, selected] : settings) {
    if (!portable_id(target_id)) {
      fail(std::string(label) + " has an invalid target identifier");
    }
    static_cast<void>(object(selected, std::string(label) + "." + target_id));
  }
}

[[nodiscard]] std::optional<std::string> optional_text(
    const CanonicalValue *value, std::string_view label,
    std::size_t maximum_bytes) {
  if (value == nullptr || value->is_null()) return std::nullopt;
  const auto *result = std::get_if<std::string>(&value->storage());
  if (result == nullptr) {
    fail(std::string(label) + " must be text");
  }
  if (result->size() > maximum_bytes) {
    fail(std::string(label) + " exceeds its byte bound");
  }
  return *result;
}

[[nodiscard]] std::optional<std::string> optional_portable_identifier(
    const CanonicalValue *value, std::string_view label) {
  if (value == nullptr || value->is_null()) return std::nullopt;
  return portable_identifier(*value, label);
}

} // namespace

ActivationDeploymentIntent parse_activation_deployment_intent(
    const CanonicalValue &value,
    std::vector<CapturedComponentModelSource> captured_sources) {
  require_fields(
      value,
      {"api_version", "attachments", "components", "deployment_artifact_id",
       "name", "policies"},
      {"assistant_profile_id", "compatibility", "description", "isaac_world"},
      "root");
  if (text(member(value, "api_version", "root"), "api_version") !=
      kApiVersion) {
    fail("has an unsupported api_version");
  }

  const auto &components = array(member(value, "components", "root"),
                                 "components");
  if (components.size() > kMaximumComponents) {
    fail("has an invalid component count");
  }
  const bool has_io = std::any_of(
      components.begin(), components.end(), [](const auto &component) {
        const auto *configuration =
            component.find_object_member("configuration");
        const auto *kind =
            configuration ? configuration->find_object_member("runtime_kind")
                          : nullptr;
        return kind && kind->is_string() && kind->as_string() == "io_provider";
      });
  if (!has_io && components.size() != captured_sources.size())
    fail("component count differs from the captured source closure");
  const auto &attachments =
      array(member(value, "attachments", "root"), "attachments");
  if (attachments.size() > kMaximumAttachments) {
    fail("attachments exceed their bound");
  }
  for (const auto &attachment : attachments) {
    static_cast<void>(object(attachment, "attachment"));
  }
  static_cast<void>(object(member(value, "policies", "root"), "policies"));
  if (const auto *compatibility = value.find_object_member("compatibility");
      compatibility != nullptr) {
    static_cast<void>(object(*compatibility, "compatibility"));
  }

  std::vector<ActivationDeploymentComponent> parsed_components;
  parsed_components.reserve(components.size());
  std::set<std::string, std::less<>> component_ids;
  std::string previous_component_id;
  std::size_t captured_index = 0U;
  for (std::size_t index = 0U; index < components.size(); ++index) {
    const auto &component = components[index];
    const std::string label = "components[" + std::to_string(index) + "]";
    const auto *raw_configuration =
        component.find_object_member("configuration");
    const auto *runtime_kind =
        raw_configuration == nullptr
            ? nullptr
            : raw_configuration->find_object_member("runtime_kind");
    const bool io_provider = runtime_kind && runtime_kind->is_string() &&
                             runtime_kind->as_string() == "io_provider";
    if (io_provider)
      require_fields(component,
                     {"component_id", "component_profile_id", "configuration",
                      "target_settings"},
                     {}, label);
    else
      require_fields(component,
                     {"component_id", "component_profile_id", "configuration",
                      "model_source", "root_pose_in_cell", "target_settings"},
                     {"frame_prefix"}, label);
    const auto &component_id = portable_identifier(
        member(component, "component_id", label), label + ".component_id");
    const auto &profile_id = portable_identifier(
        member(component, "component_profile_id", label),
        label + ".component_profile_id");
    if (!component_ids.insert(component_id).second ||
        (!previous_component_id.empty() && component_id < previous_component_id)) {
      fail("components are not uniquely canonical-sorted");
    }
    previous_component_id = component_id;
    const auto &target_settings = member(component, "target_settings", label);
    const auto &configuration = member(component, "configuration", label);
    require_target_settings(target_settings, label + ".target_settings");
    static_cast<void>(object(configuration, label + ".configuration"));
    std::optional<CapturedComponentModelSource> captured;
    std::optional<CanonicalValue> pose;
    if (io_provider) {
      require_fields(configuration, {"runtime_kind", "runtime_configuration"},
                     {}, label + ".configuration");
      if (profile_id != "io_device_v0" || !target_settings.as_object().empty())
        fail(label + " requires io_device_v0 and empty target_settings");
    } else {
      if (captured_index == captured_sources.size() ||
          captured_sources[captured_index].component_id != component_id)
        fail("component identities differ from the captured source closure");
      captured = std::move(captured_sources[captured_index++]);
      require_model_source(member(component, "model_source", label),
                           captured->srdf.has_value(), label + ".model_source");
      pose = member(component, "root_pose_in_cell", label);
      require_pose(*pose, label + ".root_pose_in_cell");
    }
    std::string frame_prefix;
    if (const auto *prefix = component.find_object_member("frame_prefix"); prefix != nullptr) {
      if (!prefix->is_string()) fail(label + ".frame_prefix must be a string");
      frame_prefix = prefix->as_string();
    }
    if (!valid_frame_prefix(frame_prefix)) {
      fail(label + ".frame_prefix must be empty or use only ASCII letters, "
           "digits, '_' and '/', without a leading '/' and with a trailing '/'");
    }
    parsed_components.push_back(
        {component_id, profile_id, pose, target_settings, configuration,
         std::move(captured), std::move(frame_prefix)});
  }

  if (captured_index != captured_sources.size())
    fail("captured source closure contains an unowned model row");
  const auto &deployment_id = portable_identifier(
      member(value, "deployment_artifact_id", "root"),
      "deployment_artifact_id");
  const auto &name = text(member(value, "name", "root"), "name");
  if (name.size() > 256U) fail("name exceeds its byte bound");

  std::optional<std::string> isaac_world;
  if (const auto *world = value.find_object_member("isaac_world")) {
    isaac_world = portable_identifier(*world, "isaac_world");
  }

  return ActivationDeploymentIntent{
      deployment_id,
      name,
      optional_text(value.find_object_member("description"), "description",
                    4096U),
      optional_portable_identifier(
          value.find_object_member("assistant_profile_id"),
          "assistant_profile_id"),
      std::move(isaac_world),
      member(value, "attachments", "root"),
      member(value, "policies", "root"),
      value.find_object_member("compatibility") == nullptr
          ? std::nullopt
          : std::optional<CanonicalValue>(
                *value.find_object_member("compatibility")),
      std::move(parsed_components),
      value,
  };
}

} // namespace manyforge::deployment::detail
