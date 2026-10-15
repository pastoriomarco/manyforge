#include "activation_component_profile_internal.hpp"

#include <cstddef>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

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
constexpr std::size_t kMaximumModelFamilies = 128U;
constexpr std::size_t kMaximumRuntimeProfiles = 256U;

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
read_description(const std::filesystem::path &capability_root) {
  const auto path = capability_root / kDescriptionRelativePath;
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

struct Profile final {
  std::string model_family_id;
  CanonicalValue value;
};

[[nodiscard]] ActivationRuntimeKind
parse_runtime_kind(const CanonicalValue &configuration,
                   std::string_view label) {
  const auto &value = text(member(configuration, "runtime_kind", label),
                           std::string(label) + ".runtime_kind");
  if (value == "ros2_control")
    return ActivationRuntimeKind::kRos2Control;
  fail(std::string(label) + ".runtime_kind is unsupported: " + value);
}

[[nodiscard]] std::string_view
runtime_kind_text(ActivationRuntimeKind) noexcept {
  return "ros2_control";
}

} // namespace

std::vector<ActivationComponentProfile> resolve_activation_component_profiles(
    const std::filesystem::path &capability_root,
    const ActivationDeploymentIntent &intent) {
  const auto parsed =
      parse_strict_json_model_yaml(read_description(capability_root));
  const auto &root = parsed.root();
  require_fields(root,
                 {"api_version", "assistant_profiles_source", "deployments",
                  "historical_program_sources", "historical_source_commit",
                  "model_families", "runtime_restoration_profiles"},
                 "description");
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
  std::set<std::string, std::less<>> model_asset_ids;
  for (std::size_t index = 0U; index < family_rows.size(); ++index) {
    const auto &row = family_rows[index];
    const std::string label =
        "description.model_families[" + std::to_string(index) + "]";
    require_fields(row,
                   {"catalog_source", "model_asset_id", "model_family_id",
                    "semantics", "source"},
                   label);
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
  }

  const auto &profile_rows =
      array(member(root, "runtime_restoration_profiles", "description"),
            "description.runtime_restoration_profiles");
  if (profile_rows.empty() || profile_rows.size() > kMaximumRuntimeProfiles) {
    fail("description has an invalid runtime-profile count");
  }
  std::map<std::string, Profile, std::less<>> profiles;
  for (std::size_t index = 0U; index < profile_rows.size(); ++index) {
    const auto &row = profile_rows[index];
    const std::string label = "description.runtime_restoration_profiles[" +
                              std::to_string(index) + "]";
    static_cast<void>(object(row, label));
    const bool has_native = row.find_object_member("native_runtime") != nullptr;
    const bool has_ros =
        row.find_object_member("ros2_control_runtime") != nullptr;
    if (has_native == has_ros) {
      fail(label + " must select exactly one runtime kind");
    }
    if (has_native) {
      require_fields(row,
                     {"acceptance_obligations", "model_family_id",
                      "native_runtime", "runtime_restoration_profile_id",
                      "source_fields", "stop_profiles"},
                     label);
      static_cast<void>(object(member(row, "native_runtime", label),
                               label + ".native_runtime"));
    } else {
      require_fields(row,
                     {"acceptance_obligations", "model_family_id",
                      "ros2_control_runtime", "runtime_restoration_profile_id",
                      "source_fields", "stop_profiles"},
                     label);
      static_cast<void>(object(member(row, "ros2_control_runtime", label),
                               label + ".ros2_control_runtime"));
    }
    static_cast<void>(
        object(member(row, "source_fields", label), label + ".source_fields"));
    static_cast<void>(
        object(member(row, "stop_profiles", label), label + ".stop_profiles"));
    static_cast<void>(array(member(row, "acceptance_obligations", label),
                            label + ".acceptance_obligations"));
    const auto &profile_id =
        text(member(row, "runtime_restoration_profile_id", label),
             label + ".runtime_restoration_profile_id");
    const auto &family_id =
        text(member(row, "model_family_id", label), label + ".model_family_id");
    if (!families.contains(family_id)) {
      fail(label + " selects unknown model family " + family_id);
    }
    if (!profiles
             .emplace(profile_id,
                      Profile{family_id, row})
             .second) {
      fail("description has duplicate runtime-profile identities");
    }
  }

  std::vector<ActivationComponentProfile> result;
  result.reserve(intent.components.size());
  for (const auto &component : intent.components) {
    const auto found = profiles.find(component.component_profile_id);
    // Retired native rows remain strictly checked opaque metadata until N4.
    // They never supply a runtime kind or qualify a selected component.
    if (found == profiles.end() ||
        found->second.value.find_object_member("native_runtime") != nullptr) {
      fail("component " + component.component_id + " selects unknown profile " +
           component.component_profile_id);
    }
    const auto authored_kind = parse_runtime_kind(
        component.configuration,
        "component " + component.component_id + ".configuration");
    if (authored_kind != ActivationRuntimeKind::kRos2Control) {
      fail("component " + component.component_id + " selects " +
           std::string(runtime_kind_text(authored_kind)) +
           " runtime configuration for " +
           std::string(runtime_kind_text(ActivationRuntimeKind::kRos2Control)) +
           " profile " + component.component_profile_id);
    }
    const auto family = families.find(found->second.model_family_id);
    if (family == families.end()) {
      throw std::logic_error("validated activation profile lost its family");
    }
    result.push_back(ActivationComponentProfile{
        component.component_id,
        component.component_profile_id,
        found->second.model_family_id,
        family->second.model_asset_id,
        family->second.accepted_package_uri_prefix,
        authored_kind,
        family->second.value,
        found->second.value,
    });
  }
  return result;
}

} // namespace manyforge::deployment::detail
